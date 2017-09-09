/*
*      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <dbt.h>
#include <math.h>
#include <Shlobj.h>
#include <Windowsx.h>

#include "Application.h"
#include "guilib/GUIControl.h"       // for EVENT_RESULT
#include "guilib/GUIWindowManager.h"
#include "input/MouseStat.h"
#include "input/InputManager.h"
#include "input/touch/generic/GenericTouchActionHandler.h"
#include "input/touch/generic/GenericTouchSwipeDetector.h"
#include "messaging/ApplicationMessenger.h"
#include "network/Zeroconf.h"
#include "network/ZeroconfBrowser.h"
#include "platform/win32/WIN32Util.h"
#include "peripherals/Peripherals.h"
#include "powermanagement/windows/Win32PowerSyscall.h"
#include "ServiceBroker.h"
#include "storage/MediaManager.h"
#include "storage/windows/Win32StorageProvider.h"
#include "Util.h"
#include "utils/JobManager.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "windowing/WindowingFactory.h"
#include "WinKeyMap.h"
#include "WinEventsWin32.h"

#ifdef TARGET_WINDOWS

using namespace KODI::MESSAGING;

HWND g_hWnd = nullptr;

#ifndef LODWORD
#define LODWORD(longval) ((DWORD)((DWORDLONG)(longval)))
#endif

#define ROTATE_ANGLE_DEGREE(arg) GID_ROTATE_ANGLE_FROM_ARGUMENT(LODWORD(arg)) * 180 / M_PI

/* Masks for processing the windows KEYDOWN and KEYUP messages */
#define REPEATED_KEYMASK  (1<<30)
#define EXTENDED_KEYMASK  (1<<24)
#define EXTKEYPAD(keypad) ((scancode & 0x100)?(mvke):(keypad))

static GUID USB_HID_GUID = { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

uint32_t g_uQueryCancelAutoPlay = 0;

int XBMC_TranslateUNICODE = 1;

int CWinEventsWin32::m_originalZoomDistance = 0;
Pointer CWinEventsWin32::m_touchPointer;
CGenericTouchSwipeDetector* CWinEventsWin32::m_touchSwipeDetector = nullptr;

// register to receive SD card events (insert/remove)
// seen at http://www.codeproject.com/Messages/2897423/Re-No-message-triggered-on-SD-card-insertion-remov.aspx
#define WM_MEDIA_CHANGE (WM_USER + 666)
SHChangeNotifyEntry shcne;

static int XBMC_MapVirtualKey(int scancode, WPARAM vkey)
{
  int mvke = MapVirtualKeyEx(scancode & 0xFF, 1, nullptr);

  switch (vkey)
  { /* These are always correct */
    case VK_DIVIDE:
    case VK_MULTIPLY:
    case VK_SUBTRACT:
    case VK_ADD:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
      /* These are already handled */
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LMENU:
    case VK_RMENU:
    case VK_SNAPSHOT:
    case VK_PAUSE:
      /* Multimedia keys are already handled */
    case VK_BROWSER_BACK:
    case VK_BROWSER_FORWARD:
    case VK_BROWSER_REFRESH:
    case VK_BROWSER_STOP:
    case VK_BROWSER_SEARCH:
    case VK_BROWSER_FAVORITES:
    case VK_BROWSER_HOME:
    case VK_VOLUME_MUTE:
    case VK_VOLUME_DOWN:
    case VK_VOLUME_UP:
    case VK_MEDIA_NEXT_TRACK:
    case VK_MEDIA_PREV_TRACK:
    case VK_MEDIA_STOP:
    case VK_MEDIA_PLAY_PAUSE:
    case VK_LAUNCH_MAIL:
    case VK_LAUNCH_MEDIA_SELECT:
    case VK_LAUNCH_APP1:
    case VK_LAUNCH_APP2:
      return vkey;
    default:;
  }
  switch (mvke)
  { /* Distinguish between keypad and extended keys */
    case VK_INSERT: return EXTKEYPAD(VK_NUMPAD0);
    case VK_DELETE: return EXTKEYPAD(VK_DECIMAL);
    case VK_END:    return EXTKEYPAD(VK_NUMPAD1);
    case VK_DOWN:   return EXTKEYPAD(VK_NUMPAD2);
    case VK_NEXT:   return EXTKEYPAD(VK_NUMPAD3);
    case VK_LEFT:   return EXTKEYPAD(VK_NUMPAD4);
    case VK_CLEAR:  return EXTKEYPAD(VK_NUMPAD5);
    case VK_RIGHT:  return EXTKEYPAD(VK_NUMPAD6);
    case VK_HOME:   return EXTKEYPAD(VK_NUMPAD7);
    case VK_UP:     return EXTKEYPAD(VK_NUMPAD8);
    case VK_PRIOR:  return EXTKEYPAD(VK_NUMPAD9);
    default:;
  }
  return mvke ? mvke : vkey;
}


static XBMC_keysym *TranslateKey(WPARAM vkey, UINT scancode, XBMC_keysym *keysym, int pressed)
{
  using namespace KODI::WINDOWING::WINDOWS;

  uint8_t keystate[256];

  /* Set the keysym information */
  keysym->scancode = static_cast<unsigned char>(scancode);
  keysym->unicode = 0;

  if ((vkey == VK_RETURN) && (scancode & 0x100))
  {
    /* No VK_ code for the keypad enter key */
    keysym->sym = XBMCK_KP_ENTER;
  }
  else
  {
    keysym->sym = VK_keymap[XBMC_MapVirtualKey(scancode, vkey)];
  }

  // Attempt to convert the keypress to a UNICODE character
  GetKeyboardState(keystate);

  if (pressed && XBMC_TranslateUNICODE)
  {
    uint16_t  wchars[2];

    /* Numlock isn't taken into account in ToUnicode,
    * so we handle it as a special case here */
    if ((keystate[VK_NUMLOCK] & 1) && vkey >= VK_NUMPAD0 && vkey <= VK_NUMPAD9)
    {
      keysym->unicode = vkey - VK_NUMPAD0 + '0';
    }
    else if (ToUnicode(static_cast<UINT>(vkey), scancode, keystate, reinterpret_cast<LPWSTR>(wchars), ARRAY_SIZE(wchars), 0) > 0)
    {
      keysym->unicode = wchars[0];
    }
  }

  // Set the modifier bitmap

  uint16_t mod = static_cast<uint16_t>(XBMCKMOD_NONE);

  // If left control and right alt are down this usually means that
  // AltGr is down
  if ((keystate[VK_LCONTROL] & 0x80) && (keystate[VK_RMENU] & 0x80))
  {
    mod |= XBMCKMOD_MODE;
  }
  else
  {
    if (keystate[VK_LCONTROL] & 0x80) mod |= XBMCKMOD_LCTRL;
    if (keystate[VK_RMENU]    & 0x80) mod |= XBMCKMOD_RALT;
  }

  // Check the remaining modifiers
  if (keystate[VK_LSHIFT]   & 0x80) mod |= XBMCKMOD_LSHIFT;
  if (keystate[VK_RSHIFT]   & 0x80) mod |= XBMCKMOD_RSHIFT;
  if (keystate[VK_RCONTROL] & 0x80) mod |= XBMCKMOD_RCTRL;
  if (keystate[VK_LMENU]    & 0x80) mod |= XBMCKMOD_LALT;
  if (keystate[VK_LWIN]     & 0x80) mod |= XBMCKMOD_LSUPER;
  if (keystate[VK_RWIN]     & 0x80) mod |= XBMCKMOD_LSUPER;
  keysym->mod = static_cast<XBMCMod>(mod);

  // Return the updated keysym
  return(keysym);
}


void CWinEventsWin32::MessagePush(XBMC_Event *newEvent)
{
  g_application.OnEvent(*newEvent);
}

bool CWinEventsWin32::MessagePump()
{
  MSG  msg;
  while( PeekMessage( &msg, nullptr, 0U, 0U, PM_REMOVE ) )
  {
    TranslateMessage( &msg );
    DispatchMessage( &msg );
  }
  return true;
}

LRESULT CALLBACK CWinEventsWin32::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  XBMC_Event newEvent;
  ZeroMemory(&newEvent, sizeof(newEvent));
  static HDEVNOTIFY hDeviceNotify;

#if 0
  if (uMsg == WM_NCCREATE)
  {
    // if available, enable DPI scaling of non-client portion of window (title bar, etc.) 
    if (g_Windowing.PtrEnableNonClientDpiScaling != NULL)
    {
      g_Windowing.PtrEnableNonClientDpiScaling(hWnd);
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
  }
#endif

  if (uMsg == WM_CREATE)
  {
    g_hWnd = hWnd;
    // need to set windows handle before WM_SIZE processing
    g_Windowing.SetWindow(hWnd);

    KODI::WINDOWING::WINDOWS::DIB_InitOSKeymap();

    g_uQueryCancelAutoPlay = RegisterWindowMessage(TEXT("QueryCancelAutoPlay"));
    shcne.pidl = nullptr;
    shcne.fRecursive = TRUE;
    long fEvents = SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED | SHCNE_MEDIAREMOVED | SHCNE_MEDIAINSERTED;
    SHChangeNotifyRegister(hWnd, SHCNRF_ShellLevel | SHCNRF_NewDelivery, fEvents, WM_MEDIA_CHANGE, 1, &shcne);
    RegisterDeviceInterfaceToHwnd(USB_HID_GUID, hWnd, &hDeviceNotify);
    return 0;
  }

  if (uMsg == WM_DESTROY)
    g_hWnd = nullptr;

  if(g_uQueryCancelAutoPlay != 0 && uMsg == g_uQueryCancelAutoPlay)
    return S_FALSE;

  switch (uMsg)
  {
    case WM_CLOSE:
    case WM_QUIT:
    case WM_DESTROY:
      if (hDeviceNotify)
      {
        if (UnregisterDeviceNotification(hDeviceNotify))
          hDeviceNotify = nullptr;
        else
          CLog::Log(LOGNOTICE, "%s: UnregisterDeviceNotification failed (%d)", __FUNCTION__, GetLastError());
      }
      newEvent.type = XBMC_QUIT;
      g_application.OnEvent(newEvent);
      break;
    case WM_SHOWWINDOW:
      {
        bool active = g_application.GetRenderGUI();
        g_application.SetRenderGUI(wParam != 0);
        if (g_application.GetRenderGUI() != active)
          g_Windowing.NotifyAppActiveChange(g_application.GetRenderGUI());
        CLog::Log(LOGDEBUG, __FUNCTION__": WM_SHOWWINDOW -> window is %s", wParam != 0 ? "shown" : "hidden");
      }
      break;
    case WM_ACTIVATE:
      {
        CLog::Log(LOGDEBUG, __FUNCTION__": WM_ACTIVATE -> window is %s", LOWORD(wParam) != WA_INACTIVE ? "active" : "inactive");
        bool active = g_application.GetRenderGUI();
        if (HIWORD(wParam))
        {
          g_application.SetRenderGUI(false);
        }
        else
        {
          WINDOWPLACEMENT lpwndpl;
          lpwndpl.length = sizeof(lpwndpl);
          if (LOWORD(wParam) != WA_INACTIVE)
          {
            if (GetWindowPlacement(hWnd, &lpwndpl))
              g_application.SetRenderGUI(lpwndpl.showCmd != SW_HIDE);
          }
          else
          {
            //g_application.SetRenderGUI(g_Windowing.WindowedMode());
          }
        }
        if (g_application.GetRenderGUI() != active)
          g_Windowing.NotifyAppActiveChange(g_application.GetRenderGUI());
        CLog::Log(LOGDEBUG, __FUNCTION__": window is %s", g_application.GetRenderGUI() ? "active" : "inactive");
      }
      break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      g_application.m_AppFocused = uMsg == WM_SETFOCUS;
      CLog::Log(LOGDEBUG, __FUNCTION__": window focus %s", g_application.m_AppFocused ? "set" : "lost");

      g_Windowing.NotifyAppFocusChange(g_application.m_AppFocused);
      if (uMsg == WM_KILLFOCUS)
      {
        std::string procfile;
        if (CWIN32Util::GetFocussedProcess(procfile))
          CLog::Log(LOGDEBUG, __FUNCTION__": Focus switched to process %s", procfile.c_str());
      }
      break;
    /* needs to be reviewed after frodo. we reset the system idle timer
       and the display timer directly now (see m_screenSaverTimer).
    case WM_SYSCOMMAND:
      switch( wParam&0xFFF0 )
      {
        case SC_MONITORPOWER:
          if (g_application.m_pPlayer->IsPlaying() || g_application.m_pPlayer->IsPausedPlayback())
            return 0;
          else if(CServiceBroker::GetSettings().GetInt(CSettings::SETTING_POWERMANAGEMENT_DISPLAYSOFF) == 0)
            return 0;
          break;
        case SC_SCREENSAVE:
          return 0;
      }
      break;*/
    case WM_SYSKEYDOWN:
      switch (wParam)
      {
        case VK_F4: //alt-f4, default event quit.
          return(DefWindowProc(hWnd, uMsg, wParam, lParam));
        case VK_RETURN: //alt-return
          if ((lParam & REPEATED_KEYMASK) == 0)
            CApplicationMessenger::GetInstance().PostMsg(TMSG_TOGGLEFULLSCREEN);
          return 0;
        default:;
      }
      //deliberate fallthrough
    case WM_KEYDOWN:
    {
      switch (wParam)
      {
        case VK_CONTROL:
          if ( lParam & EXTENDED_KEYMASK )
            wParam = VK_RCONTROL;
          else
            wParam = VK_LCONTROL;
          break;
        case VK_SHIFT:
          /* EXTENDED trick doesn't work here */
          if (GetKeyState(VK_LSHIFT) & 0x8000)
            wParam = VK_LSHIFT;
          else if (GetKeyState(VK_RSHIFT) & 0x8000)
            wParam = VK_RSHIFT;
          break;
        case VK_MENU:
          if ( lParam & EXTENDED_KEYMASK )
            wParam = VK_RMENU;
          else
            wParam = VK_LMENU;
          break;
        default:;
      }
      XBMC_keysym keysym;
      TranslateKey(wParam, HIWORD(lParam), &keysym, 1);

      newEvent.type = XBMC_KEYDOWN;
      newEvent.key.keysym = keysym;
      g_application.OnEvent(newEvent);
    }
    return(0);

    case WM_SYSKEYUP:
    case WM_KEYUP:
      {
      switch (wParam)
      {
        case VK_CONTROL:
          if ( lParam&EXTENDED_KEYMASK )
            wParam = VK_RCONTROL;
          else
            wParam = VK_LCONTROL;
          break;
        case VK_SHIFT:
          {
            uint32_t scanCodeL = MapVirtualKey(VK_LSHIFT, MAPVK_VK_TO_VSC);
            uint32_t scanCodeR = MapVirtualKey(VK_RSHIFT, MAPVK_VK_TO_VSC);
            uint32_t keyCode = static_cast<uint32_t>((lParam & 0xFF0000) >> 16);
            if (keyCode == scanCodeL)
              wParam = VK_LSHIFT;
            else if (keyCode == scanCodeR)
              wParam = VK_RSHIFT;
          }
          break;
        case VK_MENU:
          if ( lParam&EXTENDED_KEYMASK )
            wParam = VK_RMENU;
          else
            wParam = VK_LMENU;
          break;
        default:;
      }
      XBMC_keysym keysym;
      TranslateKey(wParam, HIWORD(lParam), &keysym, 1);

      if (wParam == VK_SNAPSHOT)
        newEvent.type = XBMC_KEYDOWN;
      else
        newEvent.type = XBMC_KEYUP;
      newEvent.key.keysym = keysym;
      g_application.OnEvent(newEvent);
    }
    return(0);
    case WM_APPCOMMAND: // MULTIMEDIA keys are mapped to APPCOMMANDS
    {
      CLog::Log(LOGDEBUG, __FUNCTION__": APPCOMMAND %d", GET_APPCOMMAND_LPARAM(lParam));
      newEvent.type = XBMC_APPCOMMAND;
      newEvent.appcommand.action = GET_APPCOMMAND_LPARAM(lParam);
      if (g_application.OnEvent(newEvent))
        return TRUE;
      return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    case WM_GESTURENOTIFY:
    {
      OnGestureNotify(hWnd, lParam);
      return DefWindowProc(hWnd, WM_GESTURENOTIFY, wParam, lParam);
    }
    case WM_GESTURE:
    {
      OnGesture(hWnd, lParam);
      return 0;
    }
    case WM_SYSCHAR:
      if (wParam == VK_RETURN) //stop system beep on alt-return
        return 0;
      break;
    case WM_SETCURSOR:
      if (HTCLIENT != LOWORD(lParam))
        g_Windowing.ShowOSMouse(true);
      break;
    case WM_MOUSEMOVE:
      newEvent.type = XBMC_MOUSEMOTION;
      newEvent.motion.x = GET_X_LPARAM(lParam);
      newEvent.motion.y = GET_Y_LPARAM(lParam);
      g_application.OnEvent(newEvent);
      return(0);
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
      newEvent.type = XBMC_MOUSEBUTTONDOWN;
      newEvent.button.x = GET_X_LPARAM(lParam);
      newEvent.button.y = GET_Y_LPARAM(lParam);
      newEvent.button.button = 0;
      if (uMsg == WM_LBUTTONDOWN) newEvent.button.button = XBMC_BUTTON_LEFT;
      else if (uMsg == WM_MBUTTONDOWN) newEvent.button.button = XBMC_BUTTON_MIDDLE;
      else if (uMsg == WM_RBUTTONDOWN) newEvent.button.button = XBMC_BUTTON_RIGHT;
      g_application.OnEvent(newEvent);
      return(0);
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
      newEvent.type = XBMC_MOUSEBUTTONUP;
      newEvent.button.x = GET_X_LPARAM(lParam);
      newEvent.button.y = GET_Y_LPARAM(lParam);
      newEvent.button.button = 0;
      if (uMsg == WM_LBUTTONUP) newEvent.button.button = XBMC_BUTTON_LEFT;
      else if (uMsg == WM_MBUTTONUP) newEvent.button.button = XBMC_BUTTON_MIDDLE;
      else if (uMsg == WM_RBUTTONUP) newEvent.button.button = XBMC_BUTTON_RIGHT;
      g_application.OnEvent(newEvent);
      return(0);
    case WM_MOUSEWHEEL:
      {
        // SDL, which our events system is based off, sends a MOUSEBUTTONDOWN message
        // followed by a MOUSEBUTTONUP message.  As this is a momentary event, we just
        // react on the MOUSEBUTTONUP message, resetting the state after processing.
        newEvent.type = XBMC_MOUSEBUTTONDOWN;
        // the coordinates in WM_MOUSEWHEEL are screen, not client coordinates
        POINT point;
        point.x = GET_X_LPARAM(lParam);
        point.y = GET_Y_LPARAM(lParam);
        WindowFromScreenCoords(hWnd, &point);
        newEvent.button.x = static_cast<uint16_t>(point.x);
        newEvent.button.y = static_cast<uint16_t>(point.y);
        newEvent.button.button = GET_Y_LPARAM(wParam) > 0 ? XBMC_BUTTON_WHEELUP : XBMC_BUTTON_WHEELDOWN;
        g_application.OnEvent(newEvent);
        newEvent.type = XBMC_MOUSEBUTTONUP;
        g_application.OnEvent(newEvent);
    }
      return(0);
    case WM_DPICHANGED:
    // This message tells the program that most of its window is on a
    // monitor with a new DPI. The wParam contains the new DPI, and the 
    // lParam contains a rect which defines the window rectangle scaled 
    // the new DPI. 
    {
      // get the suggested size of the window on the new display with a different DPI
      unsigned short  dpi = LOWORD(wParam);
      RECT resizeRect = *reinterpret_cast<RECT*>(lParam);
      g_Windowing.DPIChanged(dpi, resizeRect);
      return(0);
    }
    case WM_DISPLAYCHANGE:
      CLog::Log(LOGDEBUG, __FUNCTION__": display change event");  
      if (g_application.GetRenderGUI() && !g_Windowing.IsAlteringWindow() && GET_X_LPARAM(lParam) > 0 && GET_Y_LPARAM(lParam) > 0)  
      {
        g_Windowing.UpdateResolutions();
      }
      return(0);  
    case WM_SIZE:
      if (wParam == SIZE_MINIMIZED)
      {
        if (!g_Windowing.IsMinimized())
        {
          g_Windowing.SetMinimized(true);
          if (!g_application.GetRenderGUI())
            g_application.SetRenderGUI(false);
        }
      }
      else if (g_Windowing.IsMinimized())
      {
        g_Windowing.SetMinimized(false);
        if (!g_application.GetRenderGUI())
          g_application.SetRenderGUI(true);
      } 
      else
      {
        newEvent.type = XBMC_VIDEORESIZE;
        newEvent.resize.w = GET_X_LPARAM(lParam);
        newEvent.resize.h = GET_Y_LPARAM(lParam);

        CLog::Log(LOGDEBUG, __FUNCTION__": window resize event %d x %d", newEvent.resize.w, newEvent.resize.h);
        // tell device about new size
        g_Windowing.OnResize(newEvent.resize.w, newEvent.resize.h);
        // tell application about size changes
        if (g_application.GetRenderGUI() && !g_Windowing.IsAlteringWindow() && newEvent.resize.w > 0 && newEvent.resize.h > 0)
          g_application.OnEvent(newEvent);
      }
      return(0);
    case WM_MOVE:
      newEvent.type = XBMC_VIDEOMOVE;
      newEvent.move.x = GET_X_LPARAM(lParam);
      newEvent.move.y = GET_Y_LPARAM(lParam);

      CLog::Log(LOGDEBUG, __FUNCTION__": window move event");

      if (g_application.GetRenderGUI() && !g_Windowing.IsAlteringWindow())
        g_application.OnEvent(newEvent);

      return(0);
    case WM_MEDIA_CHANGE:
      {
        // This event detects media changes of usb, sd card and optical media.
        // It only works if the explorer.exe process is started. Because this
        // isn't the case for all setups we use WM_DEVICECHANGE for usb and 
        // optical media because this event is also triggered without the 
        // explorer process. Since WM_DEVICECHANGE doesn't detect sd card changes
        // we still use this event only for sd.
        long lEvent;
        PIDLIST_ABSOLUTE *ppidl;
        HANDLE hLock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wParam), static_cast<DWORD>(lParam), &ppidl, &lEvent);

        if (hLock)
        {
          wchar_t drivePath[MAX_PATH+1];
          if (!SHGetPathFromIDList(ppidl[0], drivePath))
            break;

          switch(lEvent)
          {
            case SHCNE_DRIVEADD:
            case SHCNE_MEDIAINSERTED:
              if (GetDriveType(drivePath) != DRIVE_CDROM)
              {
                CLog::Log(LOGDEBUG, __FUNCTION__": Drive %s Media has arrived.", drivePath);
                CWin32StorageProvider::SetEvent();
              }
              break;

            case SHCNE_DRIVEREMOVED:
            case SHCNE_MEDIAREMOVED:
              if (GetDriveType(drivePath) != DRIVE_CDROM)
              {
                CLog::Log(LOGDEBUG, __FUNCTION__": Drive %s Media was removed.", drivePath);
                CWin32StorageProvider::SetEvent();
              }
              break;
            default:;
          }
          SHChangeNotification_Unlock(hLock);
        }
        break;
      }
    case WM_POWERBROADCAST:
      if (wParam==PBT_APMSUSPEND)
      {
        CLog::Log(LOGDEBUG,"WM_POWERBROADCAST: PBT_APMSUSPEND event was sent");
        CWin32PowerSyscall::SetOnSuspend();
      }
      else if(wParam==PBT_APMRESUMEAUTOMATIC)
      {
        CLog::Log(LOGDEBUG,"WM_POWERBROADCAST: PBT_APMRESUMEAUTOMATIC event was sent");
        CWin32PowerSyscall::SetOnResume();
      }
      break;
    case WM_DEVICECHANGE:
      {
        switch(wParam)
        {
          case DBT_DEVNODES_CHANGED:
            CServiceBroker::GetPeripherals().TriggerDeviceScan(PERIPHERALS::PERIPHERAL_BUS_USB);
            break;
          case DBT_DEVICEARRIVAL:
          case DBT_DEVICEREMOVECOMPLETE:
            if (((_DEV_BROADCAST_HEADER*) lParam)->dbcd_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            {
              CServiceBroker::GetPeripherals().TriggerDeviceScan(PERIPHERALS::PERIPHERAL_BUS_USB);
            }
            // check if an usb or optical media was inserted or removed
            if (((_DEV_BROADCAST_HEADER*) lParam)->dbcd_devicetype == DBT_DEVTYP_VOLUME)
            {
              PDEV_BROADCAST_VOLUME lpdbv = (PDEV_BROADCAST_VOLUME)((_DEV_BROADCAST_HEADER*) lParam);
              // optical medium
              if (lpdbv -> dbcv_flags & DBTF_MEDIA)
              {
                std::string strdrive = StringUtils::Format("%c:", CWIN32Util::FirstDriveFromMask(lpdbv ->dbcv_unitmask));
                if(wParam == DBT_DEVICEARRIVAL)
                {
                  CLog::Log(LOGDEBUG, __FUNCTION__": Drive %s Media has arrived.", strdrive.c_str());
                  CJobManager::GetInstance().AddJob(new CDetectDisc(strdrive, true), NULL);
                }
                else
                {
                  CLog::Log(LOGDEBUG, __FUNCTION__": Drive %s Media was removed.", strdrive.c_str());
                  CMediaSource share;
                  share.strPath = strdrive;
                  share.strName = share.strPath;
                  g_mediaManager.RemoveAutoSource(share);
                }
              }
              else
                CWin32StorageProvider::SetEvent();
            }
          default:;
        }
        break;
      }
    case WM_PAINT:
      //some other app has painted over our window, mark everything as dirty
      g_windowManager.MarkDirty();
      break;
    case BONJOUR_EVENT:
      CZeroconf::GetInstance()->ProcessResults();
      break;
    case BONJOUR_BROWSER_EVENT:
      CZeroconfBrowser::GetInstance()->ProcessResults();
      break;
    default:;
  }
  return(DefWindowProc(hWnd, uMsg, wParam, lParam));
}

void CWinEventsWin32::RegisterDeviceInterfaceToHwnd(GUID InterfaceClassGuid, HWND hWnd, HDEVNOTIFY *hDeviceNotify)
{
  DEV_BROADCAST_DEVICEINTERFACE NotificationFilter;

  ZeroMemory( &NotificationFilter, sizeof(NotificationFilter) );
  NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
  NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  NotificationFilter.dbcc_classguid = InterfaceClassGuid;

  *hDeviceNotify = RegisterDeviceNotification(
      hWnd,                       // events recipient
      &NotificationFilter,        // type of device
      DEVICE_NOTIFY_WINDOW_HANDLE // type of recipient handle
      );
}

void CWinEventsWin32::WindowFromScreenCoords(HWND hWnd, POINT *point)
{
  if (!point) return;
  RECT clientRect;
  GetClientRect(hWnd, &clientRect);
  POINT windowPos;
  windowPos.x = clientRect.left;
  windowPos.y = clientRect.top;
  ClientToScreen(hWnd, &windowPos);
  point->x -= windowPos.x;
  point->y -= windowPos.y;
}

void CWinEventsWin32::OnGestureNotify(HWND hWnd, LPARAM lParam)
{
  // convert to window coordinates
  PGESTURENOTIFYSTRUCT gn = reinterpret_cast<PGESTURENOTIFYSTRUCT>(lParam);
  POINT point = { gn->ptsLocation.x, gn->ptsLocation.y };
  WindowFromScreenCoords(hWnd, &point);

  // by default we only want twofingertap and pressandtap gestures
  // the other gestures are enabled best on supported gestures
  GESTURECONFIG gc[] = {{ GID_ZOOM, 0, GC_ZOOM},
                        { GID_ROTATE, 0, GC_ROTATE},
                        { GID_PAN, 0, GC_PAN},
                        { GID_TWOFINGERTAP, GC_TWOFINGERTAP, GC_TWOFINGERTAP },
                        { GID_PRESSANDTAP, GC_PRESSANDTAP, GC_PRESSANDTAP }};

  // send a message to see if a control wants any
  int gestures;
  if ((gestures = CGenericTouchActionHandler::GetInstance().QuerySupportedGestures(static_cast<float>(point.x), static_cast<float>(point.y))) != EVENT_RESULT_UNHANDLED)
  {
    if (gestures & EVENT_RESULT_ZOOM)
      gc[0].dwWant |= GC_ZOOM;
    if (gestures & EVENT_RESULT_ROTATE)
      gc[1].dwWant |= GC_ROTATE;
    if (gestures & EVENT_RESULT_PAN_VERTICAL)
      gc[2].dwWant |= GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY | GC_PAN_WITH_GUTTER | GC_PAN_WITH_INERTIA;
    if (gestures & EVENT_RESULT_PAN_VERTICAL_WITHOUT_INERTIA)
      gc[2].dwWant |= GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY;
    if (gestures & EVENT_RESULT_PAN_HORIZONTAL)
      gc[2].dwWant |= GC_PAN | GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY | GC_PAN_WITH_GUTTER | GC_PAN_WITH_INERTIA;
    if (gestures & EVENT_RESULT_PAN_HORIZONTAL_WITHOUT_INERTIA)
      gc[2].dwWant |= GC_PAN | GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY;
    if (gestures & EVENT_RESULT_SWIPE)
    {
      gc[2].dwWant |= GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY | GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY | GC_PAN_WITH_GUTTER;

      // create a new touch swipe detector
      m_touchSwipeDetector = new CGenericTouchSwipeDetector(&CGenericTouchActionHandler::GetInstance(), 160.0f);
    }

    gc[0].dwBlock = gc[0].dwWant ^ 0x01;
    gc[1].dwBlock = gc[1].dwWant ^ 0x01;
    gc[2].dwBlock = gc[2].dwWant ^ 0x1F;
  }
  if (g_Windowing.PtrSetGestureConfig)
    g_Windowing.PtrSetGestureConfig(hWnd, 0, 5, gc, sizeof(GESTURECONFIG));
}

void CWinEventsWin32::OnGesture(HWND hWnd, LPARAM lParam)
{
  if (!g_Windowing.PtrGetGestureInfo)
    return;

  GESTUREINFO gi = {0};
  gi.cbSize = sizeof(gi);
  g_Windowing.PtrGetGestureInfo(reinterpret_cast<HGESTUREINFO>(lParam), &gi);

  // convert to window coordinates
  POINT point = { gi.ptsLocation.x, gi.ptsLocation.y };
  WindowFromScreenCoords(hWnd, &point);

  if (gi.dwID == GID_BEGIN)
    m_touchPointer.reset();

  // if there's a "current" touch from a previous event, copy it to "last"
  if (m_touchPointer.current.valid())
    m_touchPointer.last = m_touchPointer.current;

  // set the "current" touch
  m_touchPointer.current.x = static_cast<float>(point.x);
  m_touchPointer.current.y = static_cast<float>(point.y);
  m_touchPointer.current.time = time(nullptr);

  switch (gi.dwID)
  {
  case GID_BEGIN:
    {
      // set the "down" touch
      m_touchPointer.down = m_touchPointer.current;
      m_originalZoomDistance = 0;

      CGenericTouchActionHandler::GetInstance().OnTouchGestureStart(static_cast<float>(point.x), static_cast<float>(point.y));
    }
    break;

  case GID_END:
    CGenericTouchActionHandler::GetInstance().OnTouchGestureEnd(static_cast<float>(point.x), static_cast<float>(point.y), 0.0f, 0.0f, 0.0f, 0.0f);
    break;

  case GID_PAN:
    {
      if (!m_touchPointer.moving)
        m_touchPointer.moving = true;

      // calculate the velocity of the pan gesture
      float velocityX, velocityY;
      m_touchPointer.velocity(velocityX, velocityY);

      CGenericTouchActionHandler::GetInstance().OnTouchGesturePan(m_touchPointer.current.x, m_touchPointer.current.y,
                                                          m_touchPointer.current.x - m_touchPointer.last.x, m_touchPointer.current.y - m_touchPointer.last.y,
                                                          velocityX, velocityY);

      if (m_touchSwipeDetector != nullptr)
      {
        if (gi.dwFlags & GF_BEGIN)
        {
          m_touchPointer.down = m_touchPointer.current;
          m_touchSwipeDetector->OnTouchDown(0, m_touchPointer);
        }
        else if (gi.dwFlags & GF_END)
        {
          m_touchSwipeDetector->OnTouchUp(0, m_touchPointer);

          delete m_touchSwipeDetector;
          m_touchSwipeDetector = nullptr;
        }
        else
          m_touchSwipeDetector->OnTouchMove(0, m_touchPointer);
      }
    }
    break;

  case GID_ROTATE:
    {
      if (gi.dwFlags == GF_BEGIN)
        break;

      CGenericTouchActionHandler::GetInstance().OnRotate(static_cast<float>(point.x), static_cast<float>(point.y),
                                                        -static_cast<float>(ROTATE_ANGLE_DEGREE(gi.ullArguments)));
    }
    break;

  case GID_ZOOM:
    {
      if (gi.dwFlags == GF_BEGIN)
      {
        m_originalZoomDistance = static_cast<int>(LODWORD(gi.ullArguments));
        break;
      }

      // avoid division by 0
      if (m_originalZoomDistance == 0)
        break;

      CGenericTouchActionHandler::GetInstance().OnZoomPinch(static_cast<float>(point.x), static_cast<float>(point.y),
                                                            static_cast<float>(LODWORD(gi.ullArguments)) / static_cast<float>(m_originalZoomDistance));
    }
    break;

  case GID_TWOFINGERTAP:
    CGenericTouchActionHandler::GetInstance().OnTap(static_cast<float>(point.x), static_cast<float>(point.y), 2);
    break;

  case GID_PRESSANDTAP:
  default:
    // You have encountered an unknown gesture
    break;
  }
  if(g_Windowing.PtrCloseGestureInfoHandle)
    g_Windowing.PtrCloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(lParam));
}

#endif