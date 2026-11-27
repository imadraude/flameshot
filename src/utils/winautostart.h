// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

namespace WinAutostart {

bool isProcessElevated();
bool verifyStartupLaunch();
bool setStartupLaunch(bool enable, bool allowElevationPrompt = true);

} // namespace WinAutostart
