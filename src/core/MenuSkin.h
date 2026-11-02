#pragma once
#include "imgui.h"

// 2026-11-02 menu skins: the look of the settings overlay (ImGui). A skin is a handful of base colours (background, panel, field,
// accent, text), a corner rounding, the window opacity and an optional background picture; every ImGui colour is derived from
// those, so a skin file stays short. Built in: Default, Pip-Boy, Vault-Tec. More: any <game>\Data\VegasShaders\Skins\*.toml (the Export
// button writes the current skin there as a starting point; the file format is at the top of MenuSkin.cpp). The choice is saved
// as [Main.Menu.Style] Skin.
namespace MenuSkin {
	void Apply();						// the saved skin (or Default), at startup
	void DrawBackground();				// follows the saved choice, then draws the skin's picture: right after the main window's Begin
	void DrawPicker();					// the "Skin" dropdown and Export button for the toolbar
	void DrawMenu();					// the same choice as menu items (inside the menu bar's Skin submenu), then Export
	void Release();						// the background picture's texture (shutdown)
	ImVec4 Heading(float towardText);	// the skin's accent, lightened towards its text colour by 0-1: headings and links
}
