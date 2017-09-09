#pragma once
/*
 *      Copyright (C) 2012-2013 Team XBMC
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

#include "guilib/GUIDialog.h"

namespace PVR
{
  class CGUIDialogPVRRecordingInfo : public CGUIDialog
  {
  public:
    CGUIDialogPVRRecordingInfo(void);
    ~CGUIDialogPVRRecordingInfo(void) override = default;
    bool OnMessage(CGUIMessage& message) override;
    bool OnInfo(int actionID) override;
    bool HasListItems() const override { return true; }
    CFileItemPtr GetCurrentListItem(int offset = 0) override;

    void SetRecording(const CFileItem *item);

    static void ShowFor(const CFileItemPtr& item);

  private:
    bool OnClickButtonOK(CGUIMessage &message);
    bool OnClickButtonPlay(CGUIMessage &message);

    CFileItemPtr m_recordItem;
  };
}