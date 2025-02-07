// This file is part of Notepad++ project
// Copyright (C) 2021 The Notepad++ Contributors.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.


#include <shobjidl.h>
#include <shlwapi.h>	// PathIsDirectory
#ifdef __MINGW32__
#include <cwchar>
#endif
#include <comdef.h>		// _com_error
#include <comip.h>		// _com_ptr_t

#include "CustomFileDialog.h"
#include "Parameters.h"

// Workaround for MinGW because its implementation of __uuidof is different.
template<class T>
struct ComTraits
{
	static const GUID uid;
};
template<class T>
const GUID ComTraits<T>::uid = __uuidof(T);

// Smart pointer alias for COM objects that makes reference counting easier.
template<class T>
using com_ptr = _com_ptr_t<_com_IIID<T, &ComTraits<T>::uid>>;


namespace // anonymous
{
	// Note: these common functions could be moved to some header.

	struct Filter
	{
		generic_string name;
		generic_string ext;
	};

	static const int IDC_FILE_CUSTOM_CHECKBOX = 4;
	static const int IDC_FILE_TYPE_CHECKBOX = IDC_FILE_CUSTOM_CHECKBOX + 1;

	// Returns a first extension from the extension specification string.
	// Multiple extensions are separated with ';'.
	// Example: input - ".c;.cpp;.h", output - ".c"
	generic_string get1stExt(const generic_string& extSpec)
	{
		size_t pos = extSpec.find('.');
		if (pos != generic_string::npos)
		{
			size_t posEnd = extSpec.find(';', pos + 1);
			if (posEnd != generic_string::npos)
			{
				size_t extLen = posEnd - pos;
				return extSpec.substr(pos, extLen);
			}
			return extSpec.substr(pos);
		}
		return {};
	}

	bool replaceExt(generic_string& name, const generic_string& ext)
	{
		if (!name.empty() && !ext.empty())
		{
			// Remove an existing extension from the name.
			size_t posNameExt = name.find_last_of('.');
			if (posNameExt != generic_string::npos)
				name.erase(posNameExt);
			// Append a new extension.
			name += ext;
			return true;
		}
		return false;
	}

	bool hasExt(const generic_string& name)
	{
		return name.find_last_of('.') != generic_string::npos;
	}

	bool endsWith(const generic_string& s, const generic_string& suffix)
	{
#if defined(_MSVC_LANG) && (_MSVC_LANG > 201402L)
	#error Replace this function with basic_string::ends_with
#endif
		size_t pos = s.find(suffix);
		return pos != s.npos && ((s.length() - pos) == suffix.length());
	}

	void expandEnv(generic_string& s)
	{
		TCHAR buffer[MAX_PATH] = { 0 };
		// This returns the resulting string length or 0 in case of error.
		DWORD ret = ExpandEnvironmentStrings(s.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
		if (ret != 0)
		{
			if (ret == static_cast<DWORD>(lstrlen(buffer) + 1))
			{
				s = buffer;
			}
			else
			{
				// Buffer was too small, try with a bigger buffer of the required size.
				std::vector<TCHAR> buffer2(ret, 0);
				ret = ExpandEnvironmentStrings(s.c_str(), buffer2.data(), static_cast<DWORD>(buffer2.size()));
				assert(ret == static_cast<DWORD>(lstrlen(buffer2.data()) + 1));
				s = buffer2.data();
			}
		}
	}

	generic_string getFilename(IShellItem* psi)
	{
		generic_string result;
		if (psi)
		{
			PWSTR pszFilePath = nullptr;
			HRESULT hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
			if (SUCCEEDED(hr) && pszFilePath)
			{
				result = pszFilePath;
				CoTaskMemFree(pszFilePath);
			}
		}
		return result;
	}

	bool setDialogFolder(IFileDialog* dialog, const TCHAR* folder)
	{
		IShellItem* psi = nullptr;
		HRESULT hr = SHCreateItemFromParsingName(folder,
			0,
			IID_IShellItem,
			reinterpret_cast<void**>(&psi));
		if (SUCCEEDED(hr))
			hr = dialog->SetFolder(psi);
		return SUCCEEDED(hr);
	}

	generic_string getDialogFileName(IFileDialog* dialog)
	{
		generic_string fileName;
		if (dialog)
		{
			PWSTR pszFilePath = nullptr;
			HRESULT hr = dialog->GetFileName(&pszFilePath);
			if (SUCCEEDED(hr) && pszFilePath)
			{
				fileName = pszFilePath;
				CoTaskMemFree(pszFilePath);
			}
		}
		return fileName;
	}

	generic_string getDialogFolder(IFileDialog* dialog)
	{
		com_ptr<IShellItem> psi;
		HRESULT hr = dialog->GetFolder(&psi);
		if (SUCCEEDED(hr))
			return getFilename(psi);
		return {};
	}

	// Backs up the current directory in constructor and restores it in destructor.
	// This is needed in case dialog changes the current directory.
	class CurrentDirBackup
	{
	public:
		CurrentDirBackup()
		{
			::GetCurrentDirectory(MAX_PATH, _dir);
		}
		~CurrentDirBackup()
		{
			NppParameters& params = NppParameters::getInstance();
			if (params.getNppGUI()._openSaveDir == dir_last)
			{
				::GetCurrentDirectory(MAX_PATH, _dir);
				params.setWorkingDir(_dir);
			}
			::SetCurrentDirectory(_dir);
		}
	private:
		TCHAR _dir[MAX_PATH];
	};

} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////

class FileDialogEventHandler : public IFileDialogEvents, public IFileDialogControlEvents
{
public:
	static HRESULT createInstance(IFileDialog* dlg, const std::vector<Filter>& filterSpec,
		int fileIndex, int wildcardIndex, REFIID riid, void **ppv)
	{
		*ppv = nullptr;
		FileDialogEventHandler* pDialogEventHandler =
			new (std::nothrow) FileDialogEventHandler(dlg, filterSpec, fileIndex, wildcardIndex);
		HRESULT hr = pDialogEventHandler ? S_OK : E_OUTOFMEMORY;
		if (SUCCEEDED(hr))
		{
			hr = pDialogEventHandler->QueryInterface(riid, ppv);
			pDialogEventHandler->Release();
		}
		return hr;
	}

	// IUnknown methods

	IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		// Always set out parameter to NULL, validating it first.
		if (!ppv)
			return E_INVALIDARG;
		*ppv = nullptr;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IFileDialogEvents))
		{
			// Increment the reference count and return the pointer.
			*ppv = static_cast<IFileDialogEvents*>(this);
			AddRef();
			return NOERROR;
		}
		else if (riid == __uuidof(IFileDialogControlEvents))
		{
			// Increment the reference count and return the pointer.
			*ppv = static_cast<IFileDialogControlEvents*>(this);
			AddRef();
			return NOERROR;
		}
		return E_NOINTERFACE;
	}

	IFACEMETHODIMP_(ULONG) AddRef() override
	{
		return InterlockedIncrement(&_cRef);
	}

	IFACEMETHODIMP_(ULONG) Release() override
	{
		long cRef = InterlockedDecrement(&_cRef);
		if (!cRef)
			delete this;
		return cRef;
	}

	// IFileDialogEvents methods

	IFACEMETHODIMP OnFileOk(IFileDialog*) override
	{
		return S_OK;
	}
	IFACEMETHODIMP OnFolderChange(IFileDialog*) override
	{
		// First launch order: 3. Custom controls are added but inactive.
		return S_OK;
	}
	IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override
	{
		// First launch order: 2. Buttons are added, correct window title.
		return S_OK;
	}
	IFACEMETHODIMP OnSelectionChange(IFileDialog*) override
	{
		// First launch order: 4. Main window is shown.
		if (shouldInitControls())
			initControls();
		return S_OK;
	}
	IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override
	{
		return S_OK;
	}
	IFACEMETHODIMP OnTypeChange(IFileDialog*) override
	{
		// First launch order: 1. Inactive, window title might be wrong.
		UINT dialogIndex = 0;
		if (SUCCEEDED(_dialog->GetFileTypeIndex(&dialogIndex)))
		{
			// Enable checkbox if type was changed.
			if (OnTypeChange(dialogIndex))
				_customize->SetCheckButtonState(IDC_FILE_TYPE_CHECKBOX, TRUE);
		}
		return S_OK;
	}

	bool OnTypeChange(UINT dialogIndex)
	{
		if (dialogIndex == 0)
			return false;
		generic_string name = getDialogFileName(_dialog);
		if (changeExt(name, dialogIndex - 1))
			return SUCCEEDED(_dialog->SetFileName(name.c_str()));
		return false;
	}

	IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override
	{
		return S_OK;
	}

	// IFileDialogControlEvents methods

	IFACEMETHODIMP OnItemSelected(IFileDialogCustomize*, DWORD, DWORD) override
	{
		return E_NOTIMPL;
	}

	IFACEMETHODIMP OnButtonClicked(IFileDialogCustomize*, DWORD) override
	{
		return E_NOTIMPL;
	}

	IFACEMETHODIMP OnCheckButtonToggled(IFileDialogCustomize*, DWORD id, BOOL bChecked) override
	{
		if (id == IDC_FILE_TYPE_CHECKBOX)
		{
			bool ok = false;
			if (bChecked)
			{
				ok = SUCCEEDED(_dialog->SetFileTypeIndex(_lastSelectedType));
				ok &= OnTypeChange(_lastSelectedType);
			}
			else
			{
				UINT currentIndex = 0;
				ok = SUCCEEDED(_dialog->GetFileTypeIndex(&currentIndex));
				if (ok && currentIndex != 0 && currentIndex != _wildcardType)
					_lastSelectedType = currentIndex;
				ok &= SUCCEEDED(_dialog->SetFileTypeIndex(_wildcardType));
			}
			return S_OK;
		}
		return E_NOTIMPL;
	}

	IFACEMETHODIMP OnControlActivating(IFileDialogCustomize*, DWORD) override
	{
		return E_NOTIMPL;
	}

private:

	// Use createInstance() instead
	FileDialogEventHandler(IFileDialog* dlg, const std::vector<Filter>& filterSpec, int fileIndex, int wildcardIndex)
		: _cRef(1), _dialog(dlg), _customize(dlg), _filterSpec(filterSpec), _lastSelectedType(fileIndex + 1),
		_wildcardType(wildcardIndex >= 0 ? wildcardIndex + 1 : 0)
	{
		_staticThis = this;
	}
	~FileDialogEventHandler()
	{
		_staticThis = nullptr;
	}
	FileDialogEventHandler(const FileDialogEventHandler&) = delete;
	FileDialogEventHandler& operator=(const FileDialogEventHandler&) = delete;
	FileDialogEventHandler(FileDialogEventHandler&&) = delete;
	FileDialogEventHandler& operator=(FileDialogEventHandler&&) = delete;

	// Overrides window procedures for file name edit and ok button.
	// Call this as late as possible to ensure all the controls of the dialog are created.
	void initControls()
	{
		_okButtonProc = nullptr;
		_fileNameProc = nullptr;
		assert(_dialog);
		com_ptr<IOleWindow> pOleWnd = _dialog;
		if (pOleWnd)
		{
			HWND hwndDlg = nullptr;
			HRESULT hr = pOleWnd->GetWindow(&hwndDlg);
			if (SUCCEEDED(hr) && hwndDlg)
			{
				EnumChildWindows(hwndDlg, &EnumChildProc, 0);
			}
		}
	}

	bool shouldInitControls() const
	{
		return !_okButtonProc && !_fileNameProc;
	}

	// Changes the name extension according to currently selected file type index.
	bool changeExt(generic_string& name)
	{
		if (!_dialog)
			return false;
		UINT dialogIndex = 0;
		if (FAILED(_dialog->GetFileTypeIndex(&dialogIndex)) || dialogIndex == 0)
			return false;
		return changeExt(name, dialogIndex - 1);
	}

	bool changeExt(generic_string& name, size_t extIndex)
	{
		if (extIndex >= 0 && extIndex < _filterSpec.size())
		{
			const generic_string ext = get1stExt(_filterSpec[extIndex].ext);
			if (!endsWith(ext, _T(".*")))
				return replaceExt(name, ext);
		}
		return false;
	}

	generic_string getAbsPath(const generic_string& fileName)
	{
		if (::PathIsRelative(fileName.c_str()))
		{
			TCHAR buffer[MAX_PATH] = { 0 };
			const generic_string folder = getDialogFolder(_dialog);
			LPTSTR ret = ::PathCombine(buffer, folder.c_str(), fileName.c_str());
			if (ret)
				return buffer;
		}
		return fileName;
	}

	// Modifies the file name if necesary after user confirmed input.
	// Called after the user input but before OnFileOk() and before any name validation.
	void onPreFileOk()
	{
		if (!_dialog)
			return;
		// Get the entered name.
		generic_string fileName = getDialogFileName(_dialog);
		expandEnv(fileName);
		bool nameChanged = transformPath(fileName);
		// Update the controls.
		if (!::PathIsDirectory(getAbsPath(fileName).c_str()))
		{
			// Name is a file path.
			// Add file extension if missing.
			if (!hasExt(fileName))
				nameChanged |= changeExt(fileName);
		}
		// Update the edit box text.
		// It will update the address if the path is a directory.
		if (nameChanged)
		{
			// Clear the name first to ensure it's updated properly.
			_dialog->SetFileName(_T(""));
			_dialog->SetFileName(fileName.c_str());
		}
	}

	// Transforms a forward-slash path to a canonical Windows path.
	static bool transformPath(generic_string& fileName)
	{
		if (fileName.empty())
			return false;
		bool transformed = false;
		// Replace a forward-slash with a backslash.
		std::replace_if(fileName.begin(), fileName.end(),
			[&transformed](generic_string::value_type c)
			{
				const bool eq = (c == '/');
				transformed |= eq;
				return eq;
			},
			'\\');
		return transformed;
	}

	// Enumerates the child windows of a dialog.
	// Sets up window procedure overrides for "OK" button and file name edit box.
	static BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM)
	{
		const int bufferLen = MAX_PATH;
		static TCHAR buffer[bufferLen];
		if (GetClassName(hwnd, buffer, bufferLen) != 0)
		{
			if (lstrcmpi(buffer, _T("ComboBox")) == 0)
			{
				// The edit box of interest is a child of the combo box and has empty window text.
				// Note that file type dropdown is a combo box also (but without an edit box).
				HWND hwndChild = FindWindowEx(hwnd, nullptr, _T("Edit"), _T(""));
				if (hwndChild)
				{
					_fileNameProc = (WNDPROC)SetWindowLongPtr(hwndChild, GWLP_WNDPROC, (LPARAM)&FileNameWndProc);
					_staticThis->_hwndNameEdit = hwndChild;
				}
			}
			else if (lstrcmpi(buffer, _T("Button")) == 0)
			{
				// The button of interest has a focus by default.
				LONG style = GetWindowLong(hwnd, GWL_STYLE);
				if (style & BS_DEFPUSHBUTTON)
					_okButtonProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LPARAM)&OkButtonWndProc);
			}
		}
		if (_okButtonProc && _fileNameProc)
			return FALSE;	// Found all children, stop enumeration.
		return TRUE;
	}

	static LRESULT CALLBACK OkButtonWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		// The ways to press a button:
		// 1. space/enter is pressed when the button has focus (WM_KEYDOWN)
		// 2. left mouse click on a button (WM_LBUTTONDOWN)
		// 3. Alt + S
		bool pressed = false;
		switch (msg)
		{
		case BM_SETSTATE:
			// Sent after all press events above except when press return while focused.
			pressed = (wparam == TRUE);
			break;
		case WM_GETDLGCODE:
			// Sent for the keyboard input.
			pressed = (wparam == VK_RETURN);
			break;
		}
		if (pressed)
			_staticThis->onPreFileOk();
		return CallWindowProc(_okButtonProc, hwnd, msg, wparam, lparam);
	}

	static LRESULT CALLBACK FileNameWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		// WM_KEYDOWN with wparam == VK_RETURN isn't delivered here.
		// So watch for the keyboard input while the control has focus.
		// Initially, the control has focus.
		// WM_SETFOCUS is sent if control regains focus after losing it.
		static bool processingReturn = false;
		switch (msg)
		{
		case WM_SETFOCUS:
			_staticThis->_monitorKeyboard = true;
			break;
		case WM_KILLFOCUS:
			_staticThis->_monitorKeyboard = false;
			break;
		}
		// Avoid unnecessary processing by polling keyboard only on some messages.
		bool checkMsg = msg > WM_USER;
		if (_staticThis->_monitorKeyboard && !processingReturn && checkMsg)
		{
			SHORT state = GetAsyncKeyState(VK_RETURN);
			if (state & 0x8000)
			{
				// Avoid re-entrance because the call might generate some messages.
				processingReturn = true;
				_staticThis->onPreFileOk();
				processingReturn = false;
			}
		}
		return CallWindowProc(_fileNameProc, hwnd, msg, wparam, lparam);
	}

	static WNDPROC _okButtonProc;
	static WNDPROC _fileNameProc;
	static FileDialogEventHandler* _staticThis;

	long _cRef;
	com_ptr<IFileDialog> _dialog;
	com_ptr<IFileDialogCustomize> _customize;
	const std::vector<Filter> _filterSpec;
	HWND _hwndNameEdit = nullptr;
	bool _monitorKeyboard = true;
	UINT _lastSelectedType = 0;
	UINT _wildcardType = 0;
};

WNDPROC FileDialogEventHandler::_okButtonProc;
WNDPROC FileDialogEventHandler::_fileNameProc;
FileDialogEventHandler* FileDialogEventHandler::_staticThis;

///////////////////////////////////////////////////////////////////////////////

// Private implementation to avoid pollution with includes and defines in header.
class CustomFileDialog::Impl
{
public:
	Impl() = default;

	~Impl() = default;

	bool init(CLSID id)
	{
		if (_dialog)
			return false; // Avoid double initialization

		// Sanitize data.
		if (_fileTypeIndex >= static_cast<int>(_filterSpec.size()))
			_fileTypeIndex = 0;

		HRESULT hr = CoCreateInstance(id,
			NULL,
			CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&_dialog));
		_customize = _dialog;

		// Init the event handler.
		// Pass the initially selected file type.
		if (SUCCEEDED(hr))
			hr = FileDialogEventHandler::createInstance(_dialog, _filterSpec, _fileTypeIndex, _wildcardIndex, IID_PPV_ARGS(&_events));

		// If "assign type" is OFF, then change the file type to *.*
		if (_enableFileTypeCheckbox && !_fileTypeCheckboxValue && _wildcardIndex >= 0)
			_fileTypeIndex = _wildcardIndex;

		if (SUCCEEDED(hr) && _title)
			hr = _dialog->SetTitle(_title);

		if (SUCCEEDED(hr))
		{
			// Do not fail the initialization if failed to set a folder.
			bool isFolderSet = false;
			if (!_initialFolder.empty())
				isFolderSet = setDialogFolder(_dialog, _initialFolder.c_str());

			if (!isFolderSet && !_fallbackFolder.empty())
				isFolderSet = setDialogFolder(_dialog, _fallbackFolder.c_str());
		}

		if (SUCCEEDED(hr) && _defExt && _defExt[0] != '\0')
			hr = _dialog->SetDefaultExtension(_defExt);

		if (SUCCEEDED(hr) && _initialFileName)
		{
			generic_string newFileName = _initialFileName;
			if (_fileTypeIndex >= 0 && _fileTypeIndex < static_cast<int>(_filterSpec.size()))
			{
				if (!hasExt(newFileName))
				{
					const generic_string ext = get1stExt(_filterSpec[_fileTypeIndex].ext);
					if (!endsWith(ext, _T(".*")))
						newFileName += ext;
				}
			}
			hr = _dialog->SetFileName(newFileName.c_str());
		}

		if (SUCCEEDED(hr) && !_filterSpec.empty())
		{
			std::vector<COMDLG_FILTERSPEC> fileTypes;
			fileTypes.reserve(_filterSpec.size());
			for (auto&& filter : _filterSpec)
				fileTypes.push_back({ filter.name.data(), filter.ext.data() });
			hr = _dialog->SetFileTypes(static_cast<UINT>(fileTypes.size()), fileTypes.data());
		}

		// The selected index should be set after the file types are set.
		if (SUCCEEDED(hr) && _fileTypeIndex >= 0)
			hr = _dialog->SetFileTypeIndex(_fileTypeIndex + 1); // This index is 1-based.

		if (_enableFileTypeCheckbox)
			addCheckbox(IDC_FILE_TYPE_CHECKBOX, _fileTypeCheckboxLabel.c_str(), _fileTypeCheckboxValue);

		if (SUCCEEDED(hr))
			return addControls();

		return false;
	}

	bool initSave()
	{
		return init(CLSID_FileSaveDialog);
	}

	bool initOpen()
	{
		return init(CLSID_FileOpenDialog);
	}

	bool addFlags(DWORD dwNewFlags)
	{
		// Before setting, always get the options first in order 
		// not to override existing options.
		DWORD dwOldFlags = 0;
		HRESULT hr = _dialog->GetOptions(&dwOldFlags);
		if (SUCCEEDED(hr))
			hr = _dialog->SetOptions(dwOldFlags | dwNewFlags);
		return SUCCEEDED(hr);
	}

	bool addControls()
	{
		if (!_customize)
			return false;
		if (_checkboxLabel && _checkboxLabel[0] != '\0')
		{
			return addCheckbox(IDC_FILE_CUSTOM_CHECKBOX, _checkboxLabel, false, _isCheckboxActive);
		}
		return true;
	}

	bool addCheckbox(int id, const TCHAR* label, bool value, bool enabled = true)
	{
		if (!_customize)
			return false;
		HRESULT hr = _customize->AddCheckButton(id, label, value ? TRUE : FALSE);
		if (SUCCEEDED(hr) && !enabled)
		{
			hr = _customize->SetControlState(id, CDCS_INACTIVE | CDCS_VISIBLE);
			return SUCCEEDED(hr);
		}
		return SUCCEEDED(hr);
	}

	bool show()
	{
		assert(_dialog);
		if (!_dialog)
			return false;

		HRESULT hr = S_OK;
		DWORD dwCookie = 0;
		if (_events)
		{
			hr = _dialog->Advise(_events, &dwCookie);
			if (FAILED(hr))
				_events.Release();
		}

		bool okPressed = false;
		if (SUCCEEDED(hr))
		{
			hr = _dialog->Show(_hwndOwner);
			okPressed = SUCCEEDED(hr);
		}

		if (_events)
			_dialog->Unadvise(dwCookie);

		return okPressed;
	}

	BOOL getCheckboxState(int id) const
	{
		if (_customize)
		{
			BOOL bChecked = FALSE;
			HRESULT hr = _customize->GetCheckButtonState(id, &bChecked);
			if (SUCCEEDED(hr))
				return bChecked;
		}
		return FALSE;
	}

	generic_string getResultFilename()
	{
		generic_string fileName;
		com_ptr<IShellItem> psiResult;
		HRESULT hr = _dialog->GetResult(&psiResult);
		if (SUCCEEDED(hr))
		{
			fileName = getFilename(psiResult);
			_hasReadonly = hasReadonlyAttr(psiResult);
		}
		return fileName;
	}

	static bool hasReadonlyAttr(IShellItem* psi)
	{
		SFGAOF attrs = 0;
		HRESULT hr = psi->GetAttributes(SFGAO_READONLY, &attrs);
		if (SUCCEEDED(hr))
			return attrs & SFGAO_READONLY;
		return false;
	}

	std::vector<generic_string> getFilenames()
	{
		std::vector<generic_string> result;
		// Only the open dialog can have multiple results.
		com_ptr<IFileOpenDialog> pfd = _dialog;
		if (pfd)
		{
			com_ptr<IShellItemArray> psiaResults;
			HRESULT hr = pfd->GetResults(&psiaResults);
			if (SUCCEEDED(hr))
			{
				DWORD count = 0;
				hr = psiaResults->GetCount(&count);
				if (SUCCEEDED(hr))
				{
					for (DWORD i = 0; i != count; ++i)
					{
						com_ptr<IShellItem> psi;
						hr = psiaResults->GetItemAt(i, &psi);
						if (SUCCEEDED(hr))
						{
							_hasReadonly |= hasReadonlyAttr(psi);
							result.push_back(getFilename(psi));
						}
					}
				}
			}
		}
		return result;
	}

	HWND _hwndOwner = nullptr;
	const TCHAR* _title = nullptr;
	const TCHAR* _defExt = nullptr;
	generic_string _initialFolder;
	generic_string _fallbackFolder;
	const TCHAR* _checkboxLabel = nullptr;
	const TCHAR* _initialFileName = nullptr;
	bool _isCheckboxActive = true;
	std::vector<Filter> _filterSpec;
	int _fileTypeIndex = -1;	// preferred file type index
	int _wildcardIndex = -1;	// *.* file type index
	bool _hasReadonly = false;	// set during the result handling
	bool _enableFileTypeCheckbox = false;
	bool _fileTypeCheckboxValue = false;	// initial value
	generic_string _fileTypeCheckboxLabel;

private:
	com_ptr<IFileDialog> _dialog;
	com_ptr<IFileDialogCustomize> _customize;
	com_ptr<IFileDialogEvents> _events;
};

///////////////////////////////////////////////////////////////////////////////

CustomFileDialog::CustomFileDialog(HWND hwnd) : _impl{std::make_unique<Impl>()}
{
	_impl->_hwndOwner = hwnd;

	NppParameters& params = NppParameters::getInstance();
	const TCHAR* workDir = params.getWorkingDir();
	if (workDir)
		_impl->_fallbackFolder = workDir;
}

CustomFileDialog::~CustomFileDialog() = default;

void CustomFileDialog::setTitle(const TCHAR* title)
{
	_impl->_title = title;
}

void CustomFileDialog::setExtFilter(const TCHAR *extText, const TCHAR *exts)
{
	// Add an asterisk before each dot in file patterns
	generic_string newExts{ exts ? exts : _T("") };
	for (size_t pos = 0; pos < newExts.size(); ++pos)
	{
		pos = newExts.find(_T('.'), pos);
		if (pos == generic_string::npos)
			break;
		if (pos == 0 || newExts[pos - 1] != _T('*'))
		{
			newExts.insert(pos, 1, _T('*'));
			++pos;
		}
	}

	if (newExts.find(_T("*.*")) == 0)
		_impl->_wildcardIndex = static_cast<int>(_impl->_filterSpec.size());

	_impl->_filterSpec.push_back({ extText, newExts });
}

void CustomFileDialog::setExtFilter(const TCHAR *extText, std::initializer_list<const TCHAR*> extList)
{
	generic_string exts;
	for (auto&& x : extList)
	{
		exts += x;
		exts += _T(';');
	}
	exts.pop_back();	// remove the last ';'
	setExtFilter(extText, exts.c_str());
}

void CustomFileDialog::setDefExt(const TCHAR* ext)
{
	_impl->_defExt = ext;
}

void CustomFileDialog::setDefFileName(const TCHAR* fn)
{
	_impl->_initialFileName = fn;
}

void CustomFileDialog::setFolder(const TCHAR* folder)
{
	_impl->_initialFolder = folder ? folder : _T("");
}

void CustomFileDialog::setCheckbox(const TCHAR* text, bool isActive)
{
	_impl->_checkboxLabel = text;
	_impl->_isCheckboxActive = isActive;
}

void CustomFileDialog::setExtIndex(int extTypeIndex)
{
	_impl->_fileTypeIndex = extTypeIndex;
}

bool CustomFileDialog::getCheckboxState() const
{
	return _impl->getCheckboxState(IDC_FILE_CUSTOM_CHECKBOX);
}

bool CustomFileDialog::isReadOnly() const
{
	return _impl->_hasReadonly;
}

void CustomFileDialog::enableFileTypeCheckbox(const generic_string& text, bool value)
{
	assert(!text.empty());
	if (!text.empty())
	{
		_impl->_fileTypeCheckboxLabel = text;
		_impl->_enableFileTypeCheckbox = true;
		_impl->_fileTypeCheckboxValue = value;
	}
}

bool CustomFileDialog::getFileTypeCheckboxValue() const
{
	return _impl->getCheckboxState(IDC_FILE_TYPE_CHECKBOX);
}

generic_string CustomFileDialog::doSaveDlg()
{
	if (!_impl->initSave())
		return {};

	CurrentDirBackup backup;

	_impl->addFlags(FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
	bool bOk = _impl->show();
	return bOk ? _impl->getResultFilename() : _T("");
}

generic_string CustomFileDialog::doOpenSingleFileDlg()
{
	if (!_impl->initOpen())
		return {};

	CurrentDirBackup backup;

	_impl->addFlags(FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
	bool bOk = _impl->show();
	return bOk ? _impl->getResultFilename() : _T("");
}

std::vector<generic_string> CustomFileDialog::doOpenMultiFilesDlg()
{
	if (!_impl->initOpen())
		return {};

	CurrentDirBackup backup;

	_impl->addFlags(FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT);
	bool bOk = _impl->show();
	if (bOk)
		return _impl->getFilenames();
	return {};
}

generic_string CustomFileDialog::pickFolder()
{
	if (!_impl->initOpen())
		return {};

	_impl->addFlags(FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS);
	bool bOk = _impl->show();
	return bOk ? _impl->getResultFilename() : _T("");
}
