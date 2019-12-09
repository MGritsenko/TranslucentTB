#include "mainappwindow.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include "taskdialogs/aboutdialog.hpp"
#include "constants.hpp"
#include "../ProgramLog/log.hpp"

void MainAppWindow::SetupFolderWatch()
{
	m_FileChangedMessage = RegisterWindowMessage(WM_FILECHANGED);
	if (!m_FileChangedMessage)
	{
		LastErrorHandle(spdlog::level::warn, L"Failed to register file changed message.");
	}

	m_FolderWatcher = wil::make_folder_change_reader(m_ConfigPath.parent_path().c_str(), false, wil::FolderChangeEvents::All, [this](wil::FolderChangeEvent, std::wstring_view file)
	{
		if (file.empty() || Util::IgnoreCaseStringEquals(file, CONFIG_FILE))
		{
			// This callback runs on another thread, so we use a message to avoid threading issues.
			send_message(m_FileChangedMessage);
		}
	});
}

void MainAppWindow::LoadStartupTask() try
{
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::ApplicationModel;
	using Collections::IVectorView;
	StartupTask::GetForCurrentPackageAsync().Completed([this](const IAsyncOperation<IVectorView<StartupTask>> &op, AsyncStatus)
	{
		try
		{
			auto result = op.GetResults().GetAt(0);

			auto lock = m_TaskLock.lock_exclusive();
			m_StartupTask = std::move(result);
		}
		HresultErrorCatch(spdlog::level::warn, L"Failed to get first startup task.");
	});
}
HresultErrorCatch(spdlog::level::warn, L"Failed to load package startup tasks.");

LRESULT MainAppWindow::MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_CLOSE:
		Exit(true);
		return 1;

	case WM_QUERYENDSESSION:
		if (lParam & ENDSESSION_CLOSEAPP)
		{
			// The app is being queried if it can close for an update.
			RegisterApplicationRestart(nullptr, 0);
		}
		return 1;

	case WM_ENDSESSION:
		if (wParam)
		{
			// The app can be closed anytime after processing this message. Save the settings.
			Save();
		}

		return 0;

	default:
		if (uMsg == m_FileChangedMessage)
		{
			m_Config = Config::Load(m_ConfigPath);
			ConfigurationReloaded();
			return 0;
		}
		else
		{
			return TrayContextMenu::MessageHandler(uMsg, wParam, lParam);
		}
	}
}

void MainAppWindow::RefreshMenu()
{
	// TODO
}

void MainAppWindow::ClickHandler(unsigned int id)
{
	if ((id & 0xFF00) >> 8 == 0x9C)
	{
		const uint8_t group_id = (id & 0xF0) >> 4;
		const uint8_t offset = id & 0xF;
		switch (group_id)
		{
		case 0x4:
			AppearanceMenuHandler(offset, m_Config.DesktopAppearance, m_Config.UseRegularAppearanceWhenPeeking);
			break;
		case 0x5:
		case 0x6:
		case 0x7:
		case 0x8:
		case 0x9:
		{
			auto &appearance = OptionalAppearanceForGroup(group_id);
			AppearanceMenuHandler(offset, appearance, appearance.Enabled);
			break;
		}
		case 0xA:
			m_Config.Peek = static_cast<PeekBehavior>(offset);
			AppearanceChanged();
			break;
		case 0xB:
			LogMenuHandler(offset);
			break;
		}
	}
	else
	{
		const uint8_t item = id & 0xFF;
		switch (item) // TODO: 0x3, 0x6
		{
		case 0x0:
			Save();
			HresultVerify(win32::EditFile(m_ConfigPath), spdlog::level::err, L"Failed to open configuration file.");
			break;
		case 0x1:
			m_Config = { };
			Save(); // Reloaded by folder watcher
			break;
		case 0x2:
			InvertBool(m_Config.DisableSaving);
			break;
		case 0x4:
			m_Worker.DumpState();
			break;
		case 0x5:
			m_Worker.ResetState();
			break;
		case 0x6:
			AboutDialog(m_HasPackageIdentity, hinstance()).Run();
			break;
		case 0x8:
			AutostartMenuHandler();
			break;
		case 0x9:
			HresultVerify(win32::OpenLink(L"https://" APP_NAME ".github.io/tips"), spdlog::level::err, L"Failed to open tips & tricks link.");
			break;
		case 0x7:
		case 0xA:
			Exit(item == 0xA);
			break;
		}
	}
}

OptionalTaskbarAppearance &MainAppWindow::OptionalAppearanceForGroup(uint8_t group_id)
{
	switch (group_id)
	{
	case 0x5: return m_Config.VisibleWindowAppearance;
	case 0x6: return m_Config.MaximisedWindowAppearance;
	case 0x7: return m_Config.StartOpenedAppearance;
	case 0x8: return m_Config.CortanaOpenedAppearance;
	case 0x9: return m_Config.TimelineOpenedAppearance;
	default:
		assert(false);
		__assume(0);
	}
}

void MainAppWindow::AppearanceMenuHandler(uint8_t offset, TaskbarAppearance &appearance, bool &b)
{
	if (offset == 0)
	{
		InvertBool(b);
		AppearanceChanged();
	}
	else if (offset == 1)
	{
		// TODO: color
	}
	else if (offset == 2)
	{
		appearance.Accent = ACCENT_NORMAL;
		AppearanceChanged();
	}
	else
	{
		appearance.Accent = static_cast<ACCENT_STATE>(offset - 2);
		AppearanceChanged();
	}
}

void MainAppWindow::LogMenuHandler(uint8_t offset)
{
	if (offset == 0)
	{
		Log::ViewFile();
	}
	else if (offset <= 2 && offset >= 5)
	{
		m_Config.LogVerbosity = static_cast<spdlog::level::level_enum>(offset - 1);
		VerbosityChanged();
	}
	else if (offset == 6)
	{
		m_Config.LogVerbosity = spdlog::level::off;
		VerbosityChanged();
	}
}

void MainAppWindow::AutostartMenuHandler() try
{
	if (!m_HasPackageIdentity)
	{
		MessagePrint(spdlog::level::err, L"No package identity!");
	}

	auto lock = m_TaskLock.lock_shared();

	if (!m_StartupTask)
	{
		MessagePrint(spdlog::level::err, L"Startup task is null.");
	}

	switch (m_StartupTask.State())
	{
		using winrt::Windows::ApplicationModel::StartupTaskState;
		using namespace winrt::Windows::Foundation;

	case StartupTaskState::Disabled:
		m_StartupTask.RequestEnableAsync().Completed([](const IAsyncOperation<StartupTaskState> &op, AsyncStatus)
		{
			try
			{
				const auto result = op.GetResults();
				if (result != StartupTaskState::Enabled &&
					result != StartupTaskState::EnabledByPolicy)
				{
					MessagePrint(spdlog::level::err, L"A request to enable the startup task did not result in it being enabled.");
				}
			}
			HresultErrorCatch(spdlog::level::err, L"Failed to enable startup task.")
		});
		break;

	case StartupTaskState::Enabled:
		m_StartupTask.Disable();
		break;

	default:
		MessagePrint(spdlog::level::err, L"Cannot change startup state because it is locked by external factors.");
		break;
	}
}
HresultErrorCatch(spdlog::level::err, L"Changing startup task state failed!")

void MainAppWindow::Exit(bool save)
{
	if (save)
	{
		Save();
	}

	PostQuitMessage(0);
}

void MainAppWindow::VerbosityChanged()
{
	Log::SetLevel(m_Config.LogVerbosity);
}

void MainAppWindow::AppearanceChanged()
{
	m_Worker.ConfigurationChanged();
}

void MainAppWindow::TrayIconChanged()
{
	if (m_Config.HideTray)
	{
		Hide();
	}
	else
	{
		Show();
	}
}

void MainAppWindow::ConfigurationReloaded()
{
	VerbosityChanged();
	TrayIconChanged();
	AppearanceChanged();
}

MainAppWindow::MainAppWindow(std::filesystem::path configPath, bool hasPackageIdentity, HINSTANCE hInstance) :
	TrayContextMenu(TRAY_GUID, TRAY_WINDOW, APP_NAME, MAKEINTRESOURCE(IDI_TRAYWHITEICON), MAKEINTRESOURCE(IDI_TRAYBLACKICON), MAKEINTRESOURCE(IDR_TRAY_MENU), hInstance),
	m_ConfigPath(std::move(configPath)),
	m_Config(Config::Load(m_ConfigPath)),
	// hack: load verbosity as early as possible
	m_Worker((VerbosityChanged(), m_Config), hInstance),
	m_FileChangedMessage(0),
	m_HasPackageIdentity(hasPackageIdentity),
	m_StartupTask(nullptr)
{
	SetupFolderWatch();

	if (m_HasPackageIdentity)
	{
		LoadStartupTask();
	}
	else
	{
		RemoveItem(ID_AUTOSTART);
	}

	if (DarkModeAvailable() && SetPreferredAppMode)
	{
		SetPreferredAppMode(PreferredAppMode::AllowDark);
	}

	// Shows the tray icon if not disabled
	TrayIconChanged();
}

WPARAM MainAppWindow::Run()
{
	while (true)
	{
		const DWORD result = MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);

		if (result == WAIT_OBJECT_0)
		{
			// TODO: pretranslate
			for (MSG msg; PeekMessage(&msg, 0, 0, 0, PM_REMOVE);)
			{
				if (msg.message != WM_QUIT)
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
				else
				{
					return msg.wParam;
				}
			}
		}
		else if (result == WAIT_FAILED)
		{
			LastErrorHandle(spdlog::level::critical, L"Failed to enter alertable wait state!");
		}
		else
		{
			MessagePrint(spdlog::level::critical, L"MsgWaitForMultipleObjectsEx returned an unexpected value!");
		}
	}
}

void MainAppWindow::CloseRemote()
{
	Window::Find(TRAY_WINDOW, APP_NAME).send_message(WM_CLOSE);
}
