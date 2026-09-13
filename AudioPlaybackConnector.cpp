#include "pch.h"
#include "AudioPlaybackConnector.h"
#include <Dbt.h>
#include <initguid.h>
#include <map> // [수정됨] 횟수 추적을 위해 set에서 map으로 변경

// 블루투스 어댑터 전원 상태를 감지하기 위한 고유 식별자(GUID)
DEFINE_GUID(GUID_BTHPORT_DEVICE_INTERFACE, 0x0850302a, 0xb344, 0x4fda, 0x9b, 0xe9, 0x90, 0x57, 0x6b, 0x8d, 0x46, 0xf0);

// [수정됨] 절전 복귀 시 오디오 버그 해결을 위해 '몇 번' 재연결할지 기록
std::map<std::wstring, int> g_wakeUpDevices;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DEV_BROADCAST_DEVICEINTERFACE_W filter = { 0 };
	filter.dbcc_size = sizeof(filter);
	filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	filter.dbcc_classguid = GUID_BTHPORT_DEVICE_INTERFACE;
	RegisterDeviceNotificationW(g_hWnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		for (const auto& connection : g_audioPlaybackConnections)
		{
			connection.second.second.Close();
			g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}
		if (g_reconnect)
		{
			SaveSettings();
			g_audioPlaybackConnections.clear();
		}
		else
		{
			g_audioPlaybackConnections.clear();
			SaveSettings();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
		}
		break;
		case WM_RBUTTONUP: 
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				if (g_audioPlaybackConnections.find(i) == g_audioPlaybackConnections.end())
				{
					ConnectDevice(g_devicePicker, i);
				}
			}
		}
		break;

	case WM_POWERBROADCAST:
		if (wParam == PBT_APMSUSPEND) 
		{
			for (const auto& connection : g_audioPlaybackConnections)
			{
				g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
				connection.second.second.Close();
			}
			g_audioPlaybackConnections.clear();
		}
		else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
		{
			for (const auto& dev : g_lastDevices) {
				g_wakeUpDevices[dev] = 2; // [수정됨] 1분 간격으로 2번 재연결 예약
			}
			SetTimer(hWnd, 9999, 60000, nullptr); // 최초 1분 대기
		}
		break;

	case WM_DEVICECHANGE:
		if (wParam == DBT_DEVICEREMOVECOMPLETE) 
		{
			for (const auto& connection : g_audioPlaybackConnections)
			{
				g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
				connection.second.second.Close();
			}
			g_audioPlaybackConnections.clear();
		}
		else if (wParam == DBT_DEVICEARRIVAL) 
		{
			for (const auto& dev : g_lastDevices) {
				g_wakeUpDevices[dev] = 2; // [수정됨] 1분 간격으로 2번 재연결 예약
			}
			SetTimer(hWnd, 9999, 60000, nullptr); // 최초 1분 대기
		}
		break;

	case WM_TIMER:
		if (wParam == 9999) 
		{
			KillTimer(hWnd, 9999); 
			PostMessageW(hWnd, WM_CONNECTDEVICE, 0, 0); 
		}
		break;

	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(settingsItem);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	int retryCount = 0;
	const int maxRetries = 3; // [수정됨] 첫 연결 + 에러 시 2번 재시도 = 총 3번 제한
	bool success = false;
	std::wstring errorMessage;

	while (retryCount < maxRetries)
	{
		picker.SetDisplayStatus(device, _((L"Connecting... (Retry " + std::to_wstring(retryCount + 1) + L")").c_str()), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

		try
		{
			auto existingIt = g_audioPlaybackConnections.find(std::wstring(device.Id()));
			if (existingIt != g_audioPlaybackConnections.end()) {
				existingIt->second.second.Close();
				g_audioPlaybackConnections.erase(existingIt);
			}

			auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
			if (connection)
			{
				g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));

				connection.StateChanged([](const auto& sender, const auto&) {
					if (sender.State() == AudioPlaybackConnectionState::Closed)
					{
						auto it = g_audioPlaybackConnections.find(std::wstring(sender.DeviceId()));
						if (it != g_audioPlaybackConnections.end())
						{
							g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
							g_audioPlaybackConnections.erase(it);
						}
						sender.Close();
					}
				});

				co_await connection.StartAsync();
				auto result = co_await connection.OpenAsync();

				if (result.Status() == AudioPlaybackConnectionOpenResultStatus::Success)
				{
					std::wstring devId(device.Id());

					// [수정됨] 연결 성공 후 2번의 예약된 재연결을 1분 간격으로 수행하여 오디오 픽스
					if (g_wakeUpDevices.find(devId) != g_wakeUpDevices.end() && g_wakeUpDevices[devId] > 0)
					{
						g_wakeUpDevices[devId]--; // 남은 재연결 횟수 차감
						connection.Close(); 
						g_audioPlaybackConnections.erase(devId);
						
						co_await winrt::resume_after(std::chrono::milliseconds(60000)); // 1분 대기 후 다시 시도
						continue; 
					}
					else
					{
						g_wakeUpDevices.erase(devId); // 모든 재연결 완료 시 초기화
					}

					success = true;
					if (std::find(g_lastDevices.begin(), g_lastDevices.end(), devId) == g_lastDevices.end())
					{
						g_lastDevices.push_back(devId);
					}
					break; 
				}
				else
				{
					success = false;
					errorMessage = _(L"Device busy or not ready");
				}
			}
		}
		catch (winrt::hresult_error const& ex)
		{
			success = false;
			errorMessage = ex.message().c_str();
		}

		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end()) {
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}

		retryCount++;
		if (retryCount < maxRetries) {
			// [수정됨] 에러로 인한 연결 실패 시에도 똑같이 1분 대기 후 재시도
			co_await winrt::resume_after(std::chrono::milliseconds(60000)); 
		}
	}

	if (success) {
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	} else {
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	ConnectDevice(picker, device);
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		ConnectDevice(sender, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		std::wstring devId(device.Id());
		
		auto it = g_audioPlaybackConnections.find(devId);
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		
		auto lastIt = std::find(g_lastDevices.begin(), g_lastDevices.end(), devId);
		if (lastIt != g_lastDevices.end()) 
		{
			g_lastDevices.erase(lastIt);
		}
		
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}
