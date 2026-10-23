// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPaneContent.h"

#include <mmsystem.h>

#include "TerminalSettingsCache.h"
#include "../../types/inc/utils.hpp"

#include "BellEventArgs.g.cpp"
#include "TerminalPaneContent.g.cpp"

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::TerminalConnection;

// Restoring Claude Code sessions (see the claude-session OSC 777 extension in AdaptDispatch).
// The Terminal only ever stores a validated session id and a local drive directory; the
// command that resumes the session is composed here, from the native claude.exe found on PATH.
namespace
{
    // The two forms _claudeResumeCommandline produces, so a restored pane whose Claude
    // has since exited is persisted as its profile's plain shell again.
    const std::wregex s_generatedPwshCommandline{ LR"(^.* -NoExit -Command "Set-Location -LiteralPath '[^']*'; & '(?:[^']|'')*' --resume [0-9a-f-]{36}"$)" };
    const std::wregex s_generatedDirectCommandline{ LR"(^"[^"]*" --resume [0-9a-f-]{36}$)" };

    bool _isGeneratedClaudeCommandline(const winrt::hstring& commandline)
    {
        return std::regex_match(commandline.c_str(), s_generatedPwshCommandline) ||
               std::regex_match(commandline.c_str(), s_generatedDirectCommandline);
    }

    std::wstring _readRegistryPath(HKEY root, const wchar_t* subKey)
    {
        DWORD size = 0;
        if (RegGetValueW(root, subKey, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
        {
            return {};
        }
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, subKey, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, value.data(), &size) != ERROR_SUCCESS)
        {
            return {};
        }
        value.resize(wcsnlen(value.c_str(), value.size()));

        const auto expandedSize = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (expandedSize == 0)
        {
            return {};
        }
        std::wstring expanded(expandedSize, L'\0');
        if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), expandedSize) == 0)
        {
            return {};
        }
        expanded.resize(wcsnlen(expanded.c_str(), expanded.size()));
        return expanded;
    }

    bool _isLocalDrivePath(const std::wstring_view path) noexcept
    {
        return path.size() >= 3 && iswalpha(til::at(path, 0)) && til::at(path, 1) == L':' && til::at(path, 2) == L'\\';
    }

    // Searches the machine and user PATH as currently stored in the registry (our own
    // environment may predate the install), absolute local entries only, so neither the
    // current directory nor a network share is ever searched.
    std::wstring _findClaudeExe()
    {
        const auto path = _readRegistryPath(HKEY_LOCAL_MACHINE, LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)") + L";" +
                          _readRegistryPath(HKEY_CURRENT_USER, L"Environment");
        for (const auto entry : til::split_iterator{ std::wstring_view{ path }, L';' })
        {
            const auto dir = til::trim(entry, L' ');
            if (!_isLocalDrivePath(dir))
            {
                continue;
            }
            std::filesystem::path candidate{ dir };
            candidate /= L"claude.exe";
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate.wstring();
            }
        }
        return {};
    }

    // Avoid touching a disconnected mapped drive on the UI thread; resuming there will
    // fail visibly in the pane instead.
    bool _isUsableDirectory(const std::wstring& dir)
    {
        const wchar_t root[]{ dir[0], L':', L'\\', L'\0' };
        if (GetDriveTypeW(&root[0]) == DRIVE_REMOTE)
        {
            return true;
        }
        const auto attributes = GetFileAttributesW(dir.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && WI_IsFlagSet(attributes, FILE_ATTRIBUTE_DIRECTORY);
    }

    // True if the PowerShell command line already chooses what to run (-Command, -File,
    // -EncodedCommand, -CommandWithArgs or any abbreviation of them).
    bool _pwshRunsSomething(const std::wstring_view arguments)
    {
        static constexpr std::array<std::wstring_view, 4> parameters{ L"command", L"file", L"encodedcommand", L"commandwithargs" };
        for (const auto rawToken : til::split_iterator{ arguments, L' ' })
        {
            auto token = rawToken;
            if (token.starts_with(L"--"))
            {
                token.remove_prefix(2);
            }
            else if (token.starts_with(L'-') || token.starts_with(L'/'))
            {
                token.remove_prefix(1);
            }
            else
            {
                continue;
            }
            if (token.empty())
            {
                continue;
            }
            for (const auto parameter : parameters)
            {
                if (token.size() <= parameter.size() && til::equals_insensitive_ascii(token, parameter.substr(0, token.size())))
                {
                    return true;
                }
            }
        }
        return false;
    }

    std::wstring _claudeResumeCommandline(const std::wstring_view baseCommandline, const std::wstring_view claudeExe, const std::wstring_view sessionId, const std::wstring_view directory)
    {
        // Split off the executable, quoted or not.
        std::wstring_view exe;
        std::wstring_view arguments;
        if (baseCommandline.starts_with(L'"'))
        {
            const auto close = baseCommandline.find(L'"', 1);
            exe = baseCommandline.substr(1, close == std::wstring_view::npos ? std::wstring_view::npos : close - 1);
            arguments = close == std::wstring_view::npos ? std::wstring_view{} : baseCommandline.substr(close + 1);
        }
        else
        {
            const auto space = baseCommandline.find(L' ');
            exe = baseCommandline.substr(0, space);
            arguments = space == std::wstring_view::npos ? std::wstring_view{} : baseCommandline.substr(space);
        }
        if (const auto slash = exe.find_last_of(L"\\/"); slash != std::wstring_view::npos)
        {
            exe.remove_prefix(slash + 1);
        }

        const auto isPowerShell = til::equals_insensitive_ascii(exe, std::wstring_view{ L"pwsh.exe" }) ||
                                  til::equals_insensitive_ascii(exe, std::wstring_view{ L"pwsh" }) ||
                                  til::equals_insensitive_ascii(exe, std::wstring_view{ L"powershell.exe" }) ||
                                  til::equals_insensitive_ascii(exe, std::wstring_view{ L"powershell" });
        if (isPowerShell && !_pwshRunsSomething(arguments))
        {
            // One quoted argument; the directory is validated to contain no quotes, and
            // single quotes in the exe path are escaped PowerShell-style. Set-Location
            // wins over a profile's -WorkingDirectory or a Set-Location in $PROFILE.
            std::wstring exeLiteral{ claudeExe };
            for (size_t i = exeLiteral.find(L'\''); i != std::wstring::npos; i = exeLiteral.find(L'\'', i + 2))
            {
                exeLiteral.insert(i, 1, L'\'');
            }
            return fmt::format(FMT_COMPILE(L"{} -NoExit -Command \"Set-Location -LiteralPath '{}'; & '{}' --resume {}\""), baseCommandline, directory, exeLiteral, sessionId);
        }
        return fmt::format(FMT_COMPILE(L"\"{}\" --resume {}"), claudeExe, sessionId);
    }
}

namespace
{
    // The command that resumes the Claude session running in this pane when the layout
    // is restored, or empty if there is nothing (valid) to resume.
    std::wstring _claudeRestoreCommandline(const Profile& profile, const TermControl& control)
    {
        if (!profile || !profile.ClaudeSessionRestore())
        {
            return {};
        }
        const auto state = control.ConnectionState();
        if (state != ConnectionState::Connected && state != ConnectionState::Connecting)
        {
            return {};
        }
        const auto sessionId = control.ClaudeSessionId();
        const std::wstring directory{ control.ClaudeSessionDirectory() };
        if (sessionId.empty() || directory.empty() || !_isUsableDirectory(directory))
        {
            return {};
        }
        // ConptyConnection expands %VARIABLES% in the command line.
        const auto claudeExe = _findClaudeExe();
        if (claudeExe.empty() || claudeExe.find_first_of(L"\"%") != std::wstring::npos)
        {
            return {};
        }
        return _claudeResumeCommandline(profile.Commandline(), claudeExe, sessionId, directory);
    }
}

namespace winrt::TerminalApp::implementation
{
    TerminalPaneContent::TerminalPaneContent(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile,
                                             const std::shared_ptr<TerminalSettingsCache>& cache,
                                             const winrt::Microsoft::Terminal::Control::TermControl& control) :
        _control{ control },
        _cache{ cache },
        _profile{ profile }
    {
        _setupControlEvents();
    }

    void TerminalPaneContent::_setupControlEvents()
    {
        _controlEvents._ConnectionStateChanged = _control.ConnectionStateChanged(winrt::auto_revoke, { this, &TerminalPaneContent::_controlConnectionStateChangedHandler });
        _controlEvents._WarningBell = _control.WarningBell(winrt::auto_revoke, { get_weak(), &TerminalPaneContent::_controlWarningBellHandler });
        _controlEvents._CloseTerminalRequested = _control.CloseTerminalRequested(winrt::auto_revoke, { get_weak(), &TerminalPaneContent::_closeTerminalRequestedHandler });
        _controlEvents._RestartTerminalRequested = _control.RestartTerminalRequested(winrt::auto_revoke, { get_weak(), &TerminalPaneContent::_restartTerminalRequestedHandler });

        _controlEvents._TitleChanged = _control.TitleChanged(winrt::auto_revoke, { get_weak(), &TerminalPaneContent::_controlTitleChanged });
        _controlEvents._TabColorChanged = _control.TabColorChanged(winrt::auto_revoke, { get_weak(), &TerminalPaneContent::_controlTabColorChanged });
        _controlEvents._SetTaskbarProgress = _control.SetTaskbarProgress(winrt::auto_revoke, { get_weak(), &TerminalPaneContent::_controlSetTaskbarProgress });
        _controlEvents._ReadOnlyChanged = _control.ReadOnlyChanged(winrt::auto_revoke, { get_weak(), &TerminalPaneContent::_controlReadOnlyChanged });
        _controlEvents._FocusFollowMouseRequested = _control.FocusFollowMouseRequested(winrt::auto_revoke, { get_weak(), &TerminalPaneContent::_controlFocusFollowMouseRequested });
        _controlEvents._ShowNotification = _control.ShowNotification(winrt::auto_revoke, { get_weak(), &TerminalPaneContent::_controlShowNotification });
    }
    void TerminalPaneContent::_removeControlEvents()
    {
        _controlEvents = {};
    }

    winrt::Windows::UI::Xaml::FrameworkElement TerminalPaneContent::GetRoot()
    {
        return _control;
    }
    winrt::Microsoft::Terminal::Control::TermControl TerminalPaneContent::GetTermControl()
    {
        return _control;
    }
    winrt::Windows::Foundation::Size TerminalPaneContent::MinimumSize()
    {
        return _control.MinimumSize();
    }
    void TerminalPaneContent::Focus(winrt::Windows::UI::Xaml::FocusState reason)
    {
        _control.Focus(reason);
    }
    void TerminalPaneContent::Close()
    {
        // We deliberately remove the event handlers before closing the control.
        // This is to prevent reentrancy issues, pointless callbacks, etc.
        _removeControlEvents();

        _control.Close();
    }

    winrt::hstring TerminalPaneContent::Icon() const
    {
        return _profile.Icon().Resolved();
    }

    Windows::Foundation::IReference<winrt::Windows::UI::Color> TerminalPaneContent::TabColor() const noexcept
    {
        return _control.TabColor();
    }

    INewContentArgs TerminalPaneContent::GetNewTerminalArgs(const BuildStartupKind kind) const
    {
        NewTerminalArgs args{};
        const auto& controlSettings = _control.Settings();

        args.Profile(::Microsoft::Console::Utils::GuidToString(_profile.Guid()));
        // If we know the user's working directory use it instead of the profile.
        if (const auto dir = _control.WorkingDirectory(); !dir.empty())
        {
            args.StartingDirectory(dir);
        }
        else
        {
            args.StartingDirectory(controlSettings.StartingDirectory());
        }
        args.TabTitle(controlSettings.StartingTitle());
        args.Commandline(controlSettings.Commandline());
        args.SuppressApplicationTitle(controlSettings.SuppressApplicationTitle());
        if (controlSettings.TabColor() || controlSettings.StartingTabColor())
        {
            til::color c;
            // StartingTabColor is prioritized over other colors
            if (const auto color = controlSettings.StartingTabColor())
            {
                c = til::color(color.Value());
            }
            else
            {
                c = til::color(controlSettings.TabColor().Value());
            }

            args.TabColor(winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>{ static_cast<winrt::Windows::UI::Color>(c) });
        }

        // TODO:GH#9800 - we used to be able to persist the color scheme that a
        // TermControl was initialized with, by name. With the change to having the
        // control own its own copy of its settings, this wasn't possible anymore.
        // It probably is once again possible, but Dustin doesn't know how to undo
        // the damage done in the ControlSettings migration.

        switch (kind)
        {
        case BuildStartupKind::Content:
        case BuildStartupKind::MovePane:
            // Only fill in the ContentId if absolutely needed. If you fill in a number
            // here (even 0), we'll serialize that number, AND treat that action as an
            // "attach existing" rather than a "create"
            args.ContentId(_control.ContentId());
            break;
        case BuildStartupKind::Persist:
        {
            if (_isGeneratedClaudeCommandline(args.Commandline()))
            {
                args.Commandline(_profile.Commandline());
            }
            if (const auto claudeCommandline = _claudeRestoreCommandline(_profile, _control); !claudeCommandline.empty())
            {
                args.Commandline(claudeCommandline);
                args.StartingDirectory(_control.ClaudeSessionDirectory());
                // No SessionId: replaying the old buffer would duplicate the conversation
                // that the resumed session prints anyway.
                break;
            }

            const auto connection = _control.Connection();
            const auto id = connection ? connection.SessionId() : winrt::guid{};
            if (id != winrt::guid{})
            {
                args.SessionId(id);
            }
            break;
        }
        default:
            break;
        }

        return args;
    }

    void TerminalPaneContent::_controlTitleChanged(const IInspectable&, const IInspectable&)
    {
        TitleChanged.raise(*this, nullptr);
    }
    void TerminalPaneContent::_controlTabColorChanged(const IInspectable&, const IInspectable&)
    {
        TabColorChanged.raise(*this, nullptr);
    }
    void TerminalPaneContent::_controlSetTaskbarProgress(const IInspectable&, const IInspectable&)
    {
        TaskbarProgressChanged.raise(*this, nullptr);
    }
    void TerminalPaneContent::_controlReadOnlyChanged(const IInspectable&, const IInspectable&)
    {
        ReadOnlyChanged.raise(*this, nullptr);
    }
    void TerminalPaneContent::_controlFocusFollowMouseRequested(const IInspectable&, const IInspectable&)
    {
        FocusRequested.raise(*this, nullptr);
    }

    void TerminalPaneContent::_controlShowNotification(const IInspectable& /*sender*/, const ShowNotificationEventArgs& args)
    {
        NotificationRequested.raise(*this, winrt::make<implementation::NotificationEventArgs>(args.Title(), args.Body()));
    }

    // Method Description:
    // - Called when our attached control is closed. Triggers listeners to our close
    //   event, if we're a leaf pane.
    // - If this was called, and we became a parent pane (due to work on another
    //   thread), this function will do nothing (allowing the control's new parent
    //   to handle the event instead).
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    safe_void_coroutine TerminalPaneContent::_controlConnectionStateChangedHandler(const winrt::Windows::Foundation::IInspectable& sender,
                                                                                   const winrt::Windows::Foundation::IInspectable& args)
    {
        ConnectionStateChanged.raise(sender, args);
        auto newConnectionState = ConnectionState::Closed;
        if (const auto coreState = sender.try_as<ICoreState>())
        {
            newConnectionState = coreState.ConnectionState();
        }

        const auto previousConnectionState = std::exchange(_connectionState, newConnectionState);
        if (newConnectionState < ConnectionState::Closed)
        {
            // Pane doesn't care if the connection isn't entering a terminal state.
            co_return;
        }

        const auto weakThis = get_weak();
        co_await wil::resume_foreground(_control.Dispatcher());
        const auto strongThis = weakThis.get();
        if (!strongThis)
        {
            co_return;
        }

        // It's possible that this event handler started being executed, scheduled
        // on the UI thread, another child got created. So our control is
        // actually no longer _our_ control, and instead could be a descendant.
        //
        // When the control's new Pane takes ownership of the control, the new
        // parent will register its own event handler. That event handler will get
        // fired after this handler returns, and will properly cleanup state.

        if (previousConnectionState < ConnectionState::Connected && newConnectionState >= ConnectionState::Failed)
        {
            // A failure to complete the connection (before it has _connected_) is not covered by "closeOnExit".
            // This is to prevent a misconfiguration (closeOnExit: always, startingDirectory: garbage) resulting
            // in Terminal flashing open and immediately closed.
            co_return;
        }

        if (_profile)
        {
            const auto mode = _profile.CloseOnExit();

            if (
                // This one is obvious: If the user asked for "always" we do just that.
                (mode == CloseOnExitMode::Always) ||
                // Otherwise, and unless the user asked for the opposite of "always",
                // close the pane when the connection closed gracefully (not failed).
                (mode != CloseOnExitMode::Never && newConnectionState == ConnectionState::Closed) ||
                // However, defterm handoff can result in Windows Terminal randomly opening which may be annoying,
                // so by default we should at least always close the pane, even if the command failed.
                // See GH #13325 for discussion.
                (mode == CloseOnExitMode::Automatic && _isDefTermSession))
            {
                CloseRequested.raise(nullptr, nullptr);
            }
        }
    }

    // Method Description:
    // - Plays a warning note when triggered by the BEL control character,
    //   using the sound configured for the "Critical Stop" system event.`
    //   This matches the behavior of the Windows Console host.
    // - Will also flash the taskbar if the bellStyle setting for this profile
    //   has the 'visual' flag set
    // Arguments:
    // - <unused>
    void TerminalPaneContent::_controlWarningBellHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                         const winrt::Windows::Foundation::IInspectable& /*eventArgs*/)
    {
        if (_profile)
        {
            // We don't want to do anything if nothing is set, so check for that first
            if (static_cast<int>(_profile.BellStyle()) != 0)
            {
                if (WI_IsFlagSet(_profile.BellStyle(), winrt::Microsoft::Terminal::Settings::Model::BellStyle::Audible))
                {
                    // Audible is set, play the sound
                    auto sounds{ _profile.BellSound() };
                    if (sounds && sounds.Size() > 0)
                    {
                        // Sound paths are resolved and validated by CascadiaSettings
                        // before we reach this point.
                        auto soundPath{ sounds.GetAt(rand() % sounds.Size()).Resolved() };
                        PlaySoundW(soundPath.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_SENTRY | SND_NODEFAULT);
                    }
                    else
                    {
                        const auto soundAlias = reinterpret_cast<LPCWSTR>(SND_ALIAS_SYSTEMHAND);
                        PlaySoundW(soundAlias, nullptr, SND_ALIAS_ID | SND_ASYNC | SND_SENTRY);
                    }
                }

                if (WI_IsFlagSet(_profile.BellStyle(), winrt::Microsoft::Terminal::Settings::Model::BellStyle::Window))
                {
                    _control.BellLightOn();
                }

                // raise the event with the bool values corresponding to the taskbar and notification flags
                BellRequested.raise(*this,
                                    *winrt::make_self<TerminalApp::implementation::BellEventArgs>(
                                        WI_IsFlagSet(_profile.BellStyle(), BellStyle::Taskbar),
                                        WI_IsFlagSet(_profile.BellStyle(), BellStyle::Notification)));
            }
        }
    }

    void TerminalPaneContent::_closeTerminalRequestedHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                             const winrt::Windows::Foundation::IInspectable& /*args*/)
    {
        CloseRequested.raise(nullptr, nullptr);
    }

    void TerminalPaneContent::_restartTerminalRequestedHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                               const winrt::Windows::Foundation::IInspectable& /*args*/)
    {
        RestartTerminalRequested.raise(*this, nullptr);
    }

    void TerminalPaneContent::UpdateSettings(const CascadiaSettings& settings,
                                             const winrt::Microsoft::Terminal::Settings::Model::WindowSettings& /*windowSettings*/)
    {
        // Reload our profile from the settings model to propagate bell mode, icon, and close on exit mode (anything that uses _profile).
        const auto profile{ settings.FindProfile(_profile.Guid()) };
        _profile = profile ? profile : settings.ProfileDefaults();

        if (const auto settings{ _cache->TryLookup(_profile) })
        {
            _control.UpdateControlSettings(settings->DefaultSettings(), settings->UnfocusedSettings());
        }
    }

    // Method Description:
    // - Should be called when this pane is created via a default terminal handoff
    // - Finalizes our configuration given the information that we have been
    //   created via default handoff
    void TerminalPaneContent::MarkAsDefterm()
    {
        _isDefTermSession = true;
    }

    winrt::Windows::UI::Xaml::Media::Brush TerminalPaneContent::BackgroundBrush()
    {
        return _control.BackgroundBrush();
    }

    float TerminalPaneContent::SnapDownToGrid(const TerminalApp::PaneSnapDirection direction, const float sizeToSnap)
    {
        return _control.SnapDimensionToGrid(direction == PaneSnapDirection::Width, sizeToSnap);
    }
    Windows::Foundation::Size TerminalPaneContent::GridUnitSize()
    {
        return _control.CharacterDimensions();
    }
}
