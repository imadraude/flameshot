// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <objbase.h>
#include <oleauto.h>
#include <sddl.h>
#include <shellapi.h>
#include <taskschd.h>
#include <wrl/client.h>

#include "winautostart.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QSettings>
#include <QString>

#include <string>
#include <vector>

namespace {

constexpr wchar_t kStartupTaskName[] = L"Flameshot";
constexpr char kLegacyRunRegPath[] =
  "HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr char kLegacyRunValueName[] = "Flameshot";
constexpr char kAppPathsRegPath[] =
  "HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App "
  "Paths";

class ComScope
{
public:
    ComScope()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            m_ok = true;
            m_shouldUninit = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            m_ok = true;
            m_shouldUninit = false;
        }
    }

    ~ComScope()
    {
        if (m_shouldUninit) {
            CoUninitialize();
        }
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool ok() const
    {
        return m_ok;
    }

private:
    bool m_ok = false;
    bool m_shouldUninit = false;
};

class Bstr
{
public:
    Bstr() = default;

    explicit Bstr(const wchar_t* str)
      : m_bstr(str ? SysAllocString(str) : nullptr)
    {}

    explicit Bstr(const QString& str)
    {
        if (!str.isNull()) {
            const std::wstring wide = str.toStdWString();
            m_bstr =
              SysAllocStringLen(wide.c_str(), static_cast<UINT>(wide.size()));
        }
    }

    ~Bstr()
    {
        reset();
    }

    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;

    BSTR get() const
    {
        return m_bstr;
    }

    BSTR* put()
    {
        reset();
        return &m_bstr;
    }

    QString toQString() const
    {
        if (!m_bstr) {
            return {};
        }
        return QString::fromWCharArray(m_bstr, SysStringLen(m_bstr));
    }

private:
    void reset()
    {
        if (m_bstr) {
            SysFreeString(m_bstr);
            m_bstr = nullptr;
        }
    }

    BSTR m_bstr = nullptr;
};

VARIANT emptyVariant()
{
    VARIANT var;
    VariantInit(&var);
    return var;
}

QString normalizeWindowsPath(const QString& path)
{
    QString trimmed = path.trimmed();
    if (trimmed.size() >= 2 && trimmed.startsWith(QLatin1Char('"')) &&
        trimmed.endsWith(QLatin1Char('"'))) {
        trimmed = trimmed.mid(1, trimmed.size() - 2);
    }
    return QDir::toNativeSeparators(trimmed);
}

QString currentUserSid()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return {};
    }

    struct HandleGuard
    {
        HANDLE handle = nullptr;
        ~HandleGuard()
        {
            if (handle) {
                CloseHandle(handle);
            }
        }
    } tokenGuard{ rawToken };

    DWORD size = 0;
    GetTokenInformation(rawToken, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return {};
    }

    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(
          rawToken, TokenUser, buffer.data(), size, &size)) {
        return {};
    }

    auto* tokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR sidStr = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidStr) || !sidStr) {
        return {};
    }

    QString result = QString::fromWCharArray(sidStr);
    LocalFree(sidStr);
    return result;
}

bool connectRootFolder(Microsoft::WRL::ComPtr<ITaskService>& service,
                       Microsoft::WRL::ComPtr<ITaskFolder>& rootFolder)
{
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler,
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&service));
    if (FAILED(hr) || !service) {
        return false;
    }

    VARIANT empty = emptyVariant();
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) {
        return false;
    }

    hr = service->GetFolder(Bstr(L"\\").get(), &rootFolder);
    return SUCCEEDED(hr) && rootFolder;
}

bool hasLegacyRunEntry()
{
    QSettings bootUpSettings(QString::fromLatin1(kLegacyRunRegPath),
                             QSettings::NativeFormat);
    return bootUpSettings.contains(QString::fromLatin1(kLegacyRunValueName));
}

void removeLegacyRunEntry()
{
    QSettings bootUpSettings(QString::fromLatin1(kLegacyRunRegPath),
                             QSettings::NativeFormat);
    bootUpSettings.remove(QString::fromLatin1(kLegacyRunValueName));
}

void updateAppPaths(bool enable)
{
    QSettings bootUpPath(QString::fromLatin1(kAppPathsRegPath),
                         QSettings::NativeFormat);
    bootUpPath.beginGroup(QStringLiteral("flameshot.exe"));
    if (enable) {
        bootUpPath.setValue(
          QStringLiteral("Path"),
          QDir::toNativeSeparators(QCoreApplication::applicationDirPath()));
    } else {
        bootUpPath.remove(QString());
    }
    bootUpPath.endGroup();
}

bool taskExists()
{
    ComScope com;
    if (!com.ok()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ITaskService> service;
    Microsoft::WRL::ComPtr<ITaskFolder> rootFolder;
    if (!connectRootFolder(service, rootFolder)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IRegisteredTask> task;
    HRESULT hr = rootFolder->GetTask(Bstr(kStartupTaskName).get(), &task);
    return SUCCEEDED(hr) && task;
}

bool registerElevatedStartupTask()
{
    ComScope com;
    if (!com.ok()) {
        return false;
    }

    const QString userSid = currentUserSid();
    if (userSid.isEmpty()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ITaskService> service;
    Microsoft::WRL::ComPtr<ITaskFolder> rootFolder;
    if (!connectRootFolder(service, rootFolder)) {
        return false;
    }

    Microsoft::WRL::ComPtr<ITaskDefinition> taskDef;
    if (FAILED(service->NewTask(0, &taskDef)) || !taskDef) {
        return false;
    }

    Bstr userSidBstr(userSid);

    Microsoft::WRL::ComPtr<IRegistrationInfo> regInfo;
    if (FAILED(taskDef->get_RegistrationInfo(&regInfo)) || !regInfo) {
        return false;
    }
    if (FAILED(regInfo->put_Author(Bstr(L"Flameshot").get())) ||
        FAILED(regInfo->put_Description(
          Bstr(L"Launch Flameshot in background at user logon with highest "
               L"privileges.")
            .get()))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IPrincipal> principal;
    if (FAILED(taskDef->get_Principal(&principal)) || !principal) {
        return false;
    }
    if (FAILED(principal->put_UserId(userSidBstr.get())) ||
        FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
        FAILED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST))) {
        return false;
    }

    Microsoft::WRL::ComPtr<ITaskSettings> settings;
    if (FAILED(taskDef->get_Settings(&settings)) || !settings) {
        return false;
    }
    if (FAILED(settings->put_Enabled(VARIANT_TRUE)) ||
        FAILED(settings->put_StartWhenAvailable(VARIANT_TRUE)) ||
        FAILED(settings->put_AllowDemandStart(VARIANT_TRUE)) ||
        FAILED(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_ExecutionTimeLimit(Bstr(L"PT0S").get())) ||
        FAILED(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW))) {
        return false;
    }

    Microsoft::WRL::ComPtr<ITriggerCollection> triggers;
    if (FAILED(taskDef->get_Triggers(&triggers)) || !triggers) {
        return false;
    }
    Microsoft::WRL::ComPtr<ITrigger> trigger;
    if (FAILED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) || !trigger) {
        return false;
    }
    Microsoft::WRL::ComPtr<ILogonTrigger> logonTrigger;
    if (FAILED(trigger.As(&logonTrigger)) || !logonTrigger) {
        return false;
    }
    if (FAILED(logonTrigger->put_Id(Bstr(L"LogonTrigger").get())) ||
        FAILED(logonTrigger->put_UserId(userSidBstr.get())) ||
        FAILED(logonTrigger->put_Enabled(VARIANT_TRUE))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IActionCollection> actions;
    if (FAILED(taskDef->get_Actions(&actions)) || !actions) {
        return false;
    }
    Microsoft::WRL::ComPtr<IAction> action;
    if (FAILED(actions->Create(TASK_ACTION_EXEC, &action)) || !action) {
        return false;
    }
    Microsoft::WRL::ComPtr<IExecAction> execAction;
    if (FAILED(action.As(&execAction)) || !execAction) {
        return false;
    }

    const QString nativeAppPath =
      normalizeWindowsPath(QCoreApplication::applicationFilePath());
    const QString nativeAppDir =
      QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    if (FAILED(execAction->put_Path(Bstr(nativeAppPath).get())) ||
        FAILED(execAction->put_WorkingDirectory(Bstr(nativeAppDir).get()))) {
        return false;
    }

    VARIANT userIdVar;
    VariantInit(&userIdVar);
    userIdVar.vt = VT_BSTR;
    userIdVar.bstrVal = SysAllocString(userSidBstr.get());
    VARIANT empty = emptyVariant();

    Microsoft::WRL::ComPtr<IRegisteredTask> registeredTask;
    HRESULT hr =
      rootFolder->RegisterTaskDefinition(Bstr(kStartupTaskName).get(),
                                         taskDef.Get(),
                                         TASK_CREATE_OR_UPDATE,
                                         userIdVar,
                                         empty,
                                         TASK_LOGON_INTERACTIVE_TOKEN,
                                         empty,
                                         &registeredTask);
    VariantClear(&userIdVar);
    if (FAILED(hr)) {
        qWarning() << "RegisterTaskDefinition failed with HRESULT: 0x"
                   << QString::number(static_cast<quint32>(hr), 16);
        return false;
    }

    removeLegacyRunEntry();
    updateAppPaths(true);
    return true;
}

bool deleteElevatedStartupTask()
{
    ComScope com;
    if (!com.ok()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ITaskService> service;
    Microsoft::WRL::ComPtr<ITaskFolder> rootFolder;
    if (!connectRootFolder(service, rootFolder)) {
        return false;
    }

    HRESULT hr = rootFolder->DeleteTask(Bstr(kStartupTaskName).get(), 0);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
        hr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        qWarning() << "DeleteTask failed with HRESULT: 0x"
                   << QString::number(static_cast<quint32>(hr), 16);
        return false;
    }

    removeLegacyRunEntry();
    updateAppPaths(false);
    return true;
}

bool runElevatedConfigCommand(bool enable)
{
    const std::wstring appPath =
      normalizeWindowsPath(QCoreApplication::applicationFilePath())
        .toStdWString();
    const std::wstring params =
      enable ? L"config --autostart true" : L"config --autostart false";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = appPath.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        return false;
    }

    DWORD waitResult = WaitForSingleObject(sei.hProcess, 30000);
    DWORD exitCode = 1;
    BOOL gotExitCode = GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);

    return waitResult == WAIT_OBJECT_0 && gotExitCode && exitCode == 0 &&
           (WinAutostart::verifyStartupLaunch() == enable);
}

} // namespace

namespace WinAutostart {

bool isProcessElevated()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(
      rawToken, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(rawToken);
    return ok && elevation.TokenIsElevated != 0;
}

bool verifyStartupLaunch()
{
    if (hasLegacyRunEntry()) {
        return false;
    }

    ComScope com;
    if (!com.ok()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ITaskService> service;
    Microsoft::WRL::ComPtr<ITaskFolder> rootFolder;
    if (!connectRootFolder(service, rootFolder)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IRegisteredTask> registeredTask;
    if (FAILED(rootFolder->GetTask(Bstr(kStartupTaskName).get(),
                                   &registeredTask)) ||
        !registeredTask) {
        return false;
    }

    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (FAILED(registeredTask->get_Enabled(&enabled)) ||
        enabled != VARIANT_TRUE) {
        return false;
    }

    Microsoft::WRL::ComPtr<ITaskDefinition> taskDef;
    if (FAILED(registeredTask->get_Definition(&taskDef)) || !taskDef) {
        return false;
    }

    Microsoft::WRL::ComPtr<IPrincipal> principal;
    if (FAILED(taskDef->get_Principal(&principal)) || !principal) {
        return false;
    }

    TASK_RUNLEVEL_TYPE runLevel = TASK_RUNLEVEL_LUA;
    if (FAILED(principal->get_RunLevel(&runLevel)) ||
        runLevel != TASK_RUNLEVEL_HIGHEST) {
        return false;
    }

    Microsoft::WRL::ComPtr<IActionCollection> actions;
    if (FAILED(taskDef->get_Actions(&actions)) || !actions) {
        return false;
    }

    long count = 0;
    if (FAILED(actions->get_Count(&count)) || count <= 0) {
        return false;
    }

    const QString expectedPath =
      normalizeWindowsPath(QCoreApplication::applicationFilePath());

    for (long i = 1; i <= count; ++i) {
        Microsoft::WRL::ComPtr<IAction> action;
        if (FAILED(actions->get_Item(i, &action)) || !action) {
            continue;
        }

        TASK_ACTION_TYPE actionType = TASK_ACTION_EXEC;
        if (FAILED(action->get_Type(&actionType)) ||
            actionType != TASK_ACTION_EXEC) {
            continue;
        }

        Microsoft::WRL::ComPtr<IExecAction> execAction;
        if (FAILED(action.As(&execAction)) || !execAction) {
            continue;
        }

        Bstr actionPath;
        if (FAILED(execAction->get_Path(actionPath.put()))) {
            continue;
        }

        if (QString::compare(normalizeWindowsPath(actionPath.toQString()),
                             expectedPath,
                             Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    return false;
}

bool setStartupLaunch(bool enable, bool allowElevationPrompt)
{
    if (isProcessElevated()) {
        return enable ? registerElevatedStartupTask()
                      : deleteElevatedStartupTask();
    }

    if (!enable && !taskExists()) {
        removeLegacyRunEntry();
        updateAppPaths(false);
        return true;
    }

    if (!allowElevationPrompt) {
        return false;
    }

    return runElevatedConfigCommand(enable);
}

} // namespace WinAutostart
