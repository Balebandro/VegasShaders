#pragma once
// UNOFFICIAL 2026-11-01: the rule that decides how the SETTING logger writes a flush of pending setting changes (SessionLog.cpp
// FlushChanges), pure so tests/setting_changes.cpp can check it without the game.
//   A tester's log showed the gap: a preset loaded a few seconds after the first frame changed ~190 settings in one frame, the logger
//   wrote 60 of them and "SETTING 131 more changes in the last minute not listed", and the settings behind his bug were in no line.
//   Rule: changes that ARRIVED TOGETHER (consecutive first-queued times at most BatchGap apart: a preset load, a settings file reload,
//   a bench shot) and number more than BatchOver are a BATCH: written as one compact "SETTING batch (N changes, game time hh:mm):
//   [section] key old->new, ... | [section] ..." block (section grouping as the SESSION header's settings list), never dropped, not
//   counted against the per-minute cap. Everything else keeps the one-line-a-change format and the cap of PerMinute lines a minute
//   (changes spread over many frames: sliders, clicks).
#include <algorithm>
#include <string>
#include <vector>

namespace SettingChanges {
	constexpr unsigned PerMinute = 60;		// single SETTING lines a minute, the rest counted ("N more changes ... not listed")
	constexpr unsigned BatchOver = 60;		// a run of more than this many changes that arrived together is a batch
	constexpr double BatchGap = 0.1;		// seconds between two consecutive arrivals that still count as "together" (a frame or two)

	struct Change {
		std::string key, before, after;		// key = "Section.Key" as the SETTING lines write it
		double first = 0.0;					// when the change was first queued (seconds)
	};

	// Splits the flushed changes into batches and single changes. Order: singles keep their arrival order; a batch keeps its own.
	inline void Split(std::vector<Change> ready, std::vector<std::vector<Change>>& batches, std::vector<Change>& singles) {
		std::stable_sort(ready.begin(), ready.end(), [](const Change& a, const Change& b) { return a.first < b.first; });
		size_t start = 0;
		auto close = [&](size_t end) {	// the run ready[start..end)
			if (end - start > BatchOver) batches.emplace_back(ready.begin() + start, ready.begin() + end);
			else singles.insert(singles.end(), ready.begin() + start, ready.begin() + end);
			start = end;
		};
		for (size_t i = 1; i < ready.size(); i++) if (ready[i].first - ready[i - 1].first > BatchGap) close(i);
		if (!ready.empty()) close(ready.size());
	}

	// A batch's items for the log: "[section] key before->after, key before->after" per section, sections and keys in name order.
	// SessionLog's LogList joins them with " | " into lines of ~900 characters.
	inline std::vector<std::string> BatchItems(std::vector<Change> batch) {
		std::stable_sort(batch.begin(), batch.end(), [](const Change& a, const Change& b) { return a.key < b.key; });
		std::vector<std::string> items;
		std::string section, group;
		for (const Change& c : batch) {
			const size_t dot = c.key.find_last_of('.');
			const std::string here = dot == std::string::npos ? std::string() : c.key.substr(0, dot);
			const std::string key = dot == std::string::npos ? c.key : c.key.substr(dot + 1);
			if (group.empty() || here != section) {
				if (!group.empty()) items.push_back(group);
				section = here;
				group = "[" + section + "] ";
			}
			else group += ", ";
			group += key + " " + c.before + "->" + c.after;
		}
		if (!group.empty()) items.push_back(group);
		return items;
	}
}
