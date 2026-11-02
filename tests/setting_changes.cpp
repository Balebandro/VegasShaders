// Checks the SETTING logger's batch rule (src/core/SettingChanges.h, UNOFFICIAL 2026-11-01): a preset load's ~190 changes in one
// frame become one "SETTING batch" block (grouped by section, never dropped), changes spread over many frames stay single lines
// under the per-minute cap, a run of exactly the cap's size is not a batch, and two batches a few seconds apart stay apart.
#include <cstdio>
#include <string>
#include <vector>
#include "../src/core/SettingChanges.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace SettingChanges;

static Change At(double t, const char* key, const char* before = "0", const char* after = "1") { return { key, before, after, t }; }

int main() {
	// 1. A preset load: 190 changes within 30 ms = one batch, nothing single.
	{
		std::vector<Change> ready;
		for (int i = 0; i < 190; i++) { char key[64]; std::snprintf(key, sizeof(key), "Shaders.Effect%d.Main.Value%d", i % 7, i); ready.push_back(At(10.0 + i * 0.00015, key)); }
		std::vector<std::vector<Change>> batches;
		std::vector<Change> singles;
		Split(ready, batches, singles);
		CHECK(batches.size() == 1 && batches[0].size() == 190 && singles.empty(), "190 changes in one frame: one batch of 190, no single lines (%u batches, %u singles)",
			(unsigned)batches.size(), (unsigned)singles.size());
	}
	// 2. 61 changes together = a batch; 60 together = single lines (the cap handles them as before).
	{
		std::vector<Change> ready;
		for (int i = 0; i < 61; i++) ready.push_back(At(5.0 + i * 0.001, ("A.B.K" + std::to_string(i)).c_str()));
		std::vector<std::vector<Change>> batches;
		std::vector<Change> singles;
		Split(ready, batches, singles);
		CHECK(batches.size() == 1 && singles.empty(), "61 changes together: a batch");
		ready.pop_back();
		batches.clear(); singles.clear();
		Split(ready, batches, singles);
		CHECK(batches.empty() && singles.size() == 60, "60 changes together: single lines (the per-minute cap's own size)");
	}
	// 3. Changes spread over many frames (one every 0.3 s, 100 of them): single lines, in arrival order, no batch.
	{
		std::vector<Change> ready;
		for (int i = 0; i < 100; i++) ready.push_back(At(i * 0.3, ("A.B.K" + std::to_string(i)).c_str()));
		std::vector<std::vector<Change>> batches;
		std::vector<Change> singles;
		Split(ready, batches, singles);
		CHECK(batches.empty() && singles.size() == 100 && singles.front().key == "A.B.K0" && singles.back().key == "A.B.K99",
			"100 changes 0.3 s apart: 100 single lines in arrival order, no batch");
	}
	// 4. A slider change, then a preset load 2 s later, then another click: the batch alone is a batch, the two others single.
	{
		std::vector<Change> ready;
		ready.push_back(At(1.0, "A.B.Slider", "0.5", "0.7"));
		for (int i = 0; i < 80; i++) ready.push_back(At(3.0 + i * 0.0002, ("P.Q.K" + std::to_string(i)).c_str()));
		ready.push_back(At(4.5, "A.B.Click", "false", "true"));
		std::vector<std::vector<Change>> batches;
		std::vector<Change> singles;
		Split(ready, batches, singles);
		CHECK(batches.size() == 1 && batches[0].size() == 80 && singles.size() == 2 && singles[0].key == "A.B.Slider" && singles[1].key == "A.B.Click",
			"a slider, a preset of 80 two seconds later, a click: one batch of 80 and two single lines");
	}
	// 5. Unsorted arrival (two threads) still forms the batch: the times decide, not the queue order.
	{
		std::vector<Change> ready;
		for (int i = 0; i < 70; i++) ready.push_back(At(2.0 + ((i * 37) % 70) * 0.0005, ("A.B.K" + std::to_string(i)).c_str()));
		std::vector<std::vector<Change>> batches;
		std::vector<Change> singles;
		Split(ready, batches, singles);
		CHECK(batches.size() == 1 && batches[0].size() == 70, "70 changes queued out of time order: one batch");
	}
	// 6. The batch's items: grouped by section, sections and keys in name order, "key before->after".
	{
		std::vector<Change> batch = { At(0, "Shaders.Bloom.Main.Strength", "1", "2"), At(0, "Shaders.AmbientOcclusion.Skylighting.Enabled", "false", "true"),
			At(0, "Shaders.AmbientOcclusion.Skylighting.Strength", "0.5", "1.5"), At(0, "Main.Main.Misc.ReportKey", "27", "28"), At(0, "NoSection", "a", "b") };
		const std::vector<std::string> items = BatchItems(batch);
		CHECK(items.size() == 4, "5 changes in 4 sections: 4 items (%u)", (unsigned)items.size());
		CHECK(items.size() == 4 && items[0] == "[Main.Main.Misc] ReportKey 27->28", "one key in a section (got '%s')", items.empty() ? "" : items[0].c_str());
		CHECK(items.size() == 4 && items[1] == "[] NoSection a->b", "a key without a section: '[] key before->after', sorted by the full key (got '%s')", items.size() > 1 ? items[1].c_str() : "");
		CHECK(items.size() == 4 && items[2] == "[Shaders.AmbientOcclusion.Skylighting] Enabled false->true, Strength 0.5->1.5",
			"two keys of one section, comma-separated, in name order (got '%s')", items.size() > 2 ? items[2].c_str() : "");
		CHECK(items.size() == 4 && items[3] == "[Shaders.Bloom.Main] Strength 1->2", "the last section (got '%s')", items.size() > 3 ? items[3].c_str() : "");
	}
	// 7. The whole preset of 190 keeps every key in the items.
	{
		std::vector<Change> batch;
		for (int i = 0; i < 190; i++) { char key[64]; std::snprintf(key, sizeof(key), "Shaders.Effect%d.Main.Value%03d", i % 7, i); batch.push_back(At(0, key)); }
		const std::vector<std::string> items = BatchItems(batch);
		unsigned found = 0;
		for (int i = 0; i < 190; i++) { char key[32]; std::snprintf(key, sizeof(key), "Value%03d 0->1", i); for (const std::string& it : items) if (it.find(key) != std::string::npos) { found++; break; } }
		CHECK(items.size() == 7 && found == 190, "190 changes over 7 sections: 7 items holding all 190 keys (%u items, %u keys)", (unsigned)items.size(), found);
	}

	std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
