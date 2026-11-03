#pragma once
#include "imgui.h"

// 2026-11-02 menu skins: the look of the settings overlay (ImGui). A skin is a handful of base colours (background, panel, field,
// accent, text, and optionally heading, border, slider and selection), a corner rounding, the window opacity and an optional
// background picture; every ImGui colour is derived from those, so a skin file stays short. Built in: Default, Pip-Boy, Vault-Tec,
// Amber Terminal, NCR, Legion, Brotherhood, Strip Neon, Midnight, High Contrast, Light. More: any <game>\Data\VegasShaders\Skins\*.toml
// (the Export button writes the current look there as a starting point; the file format is at the top of MenuSkin.cpp). The choice is
// saved as [Main.Menu.Style] Skin. Every part can also be set by hand on top of whichever skin is picked: [Main.Menu.Style] Custom<Part>
// (empty or negative = the skin's own), from View > Skin > Own colours or the Main.Menu.Style page.
namespace MenuSkin {
	void Apply();						// the saved skin (or Default), at startup
	void DrawBackground();				// follows the saved choice, then draws the skin's picture: right after the main window's Begin
	void DrawPicker();					// the "Skin" dropdown and Export button for the toolbar
	void DrawMenu();					// the same choice as menu items (inside the menu bar's Skin submenu), then Own colours and Export
	void Release();						// the background picture's texture (shutdown)
	ImVec4 Heading(float towardText);	// the skin's heading colour, lightened towards its text colour by 0-1: headings and links
	// A [Main.Menu.Style] Custom<colour> row on the settings page: a tick (own colour on/off) and a colour box, width wide.
	// Returns false (and draws nothing) for any other setting; hovered says whether the mouse is over the controls.
	bool DrawColourSetting(const char* section, const char* key, const char* value, float width, bool& hovered);
}
