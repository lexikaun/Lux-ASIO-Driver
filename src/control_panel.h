#pragma once
#include <windows.h>

void InitControlPanelInstance(HINSTANCE hDll);

// statusText: live engine status line shown in the dialog (mode, period,
// underruns), built by the driver. May be null.
void ShowControlPanel(HWND parentWindow, const wchar_t* statusText = nullptr);

bool DidSettingsChange();
void ClearSettingsChangedFlag();
