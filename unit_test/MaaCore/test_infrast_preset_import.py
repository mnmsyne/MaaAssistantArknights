"""Native Debug API regression tests; no device connection or game writes.

Run from the repository root with a Debug MaaCore in build/bin/Debug:
    python -m unittest discover -s unit_test/MaaCore -p test_infrast_preset_import.py
"""

import ctypes
import json
import os
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from maa_core_eval import CoreEval


class PresetImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # 系统临时目录；Core 持有日志句柄，close 之后再清理，失败仅告警不中断
        cls.temp = tempfile.TemporaryDirectory(prefix="maa-preset-test-")
        cls.addClassCleanup(cls._cleanup_temp, cls.temp)
        cls.directory = pathlib.Path(cls.temp.name)
        cls.core = CoreEval(user_dir=cls.directory / "logs")
        cls.addClassCleanup(cls.core.close)
        cls.core._lib.AsstStop.argtypes = [ctypes.c_void_p]
        cls.core._lib.AsstStop.restype = ctypes.c_bool

    @staticmethod
    def _cleanup_temp(temp):
        try:
            temp.cleanup()
        except OSError as error:
            print(f"WARNING: failed to clean up temp dir {temp.name}: {error}")

    def append(self, config, **params):
        path = self.directory / "schedule.json"
        path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
        self.core._lib.AsstStop(self.core._handle)
        parameters = {"mode": 10000, "preset_init": True, "filename": str(path)}
        parameters.update(params)
        return self.core._lib.AsstAppendTask(
            self.core._handle, b"Infrast", json.dumps(parameters).encode("utf-8")
        )

    @staticmethod
    def config(room=None, facility="manufacture"):
        return {"plans": [{"rooms": {facility: [room or {"operators": ["清流"]}]}}]}

    def test_local_riic_web_fixtures_register(self):
        # build/bin/Debug/debug 下按账号放置的 RIIC-Web 导出件（Account*.json）应全部可解析注册
        fixtures = sorted((ROOT / "build/bin/Debug/debug").glob("Account*.json"))
        if not fixtures:
            self.skipTest("Local RIIC-Web fixtures are unavailable")
        for path in fixtures:
            with self.subTest(fixture=path.name):
                config = json.loads(path.read_text(encoding="utf-8-sig"))
                self.assertGreater(self.append(config), 0)

    def test_editor_title_configuration_is_strict_and_selector_is_independent(self):
        tasks = json.loads(
            (ROOT / "resource/tasks/InfrastPresetImport.json").read_text(
                encoding="utf-8"
            )
        )
        editor = tasks["InfrastPresetEditorRoom"]
        self.assertEqual(editor["roi"], [260, 45, 110, 45])
        self.assertTrue(editor["fullMatch"])
        self.assertTrue(editor["withoutDet"])
        self.assertFalse(editor["useRaw"])
        self.assertEqual(editor["binThreshold"], [200, 255])
        selector = tasks["InfrastPresetSelectorRoom"]
        self.assertNotIn("baseTask", selector)
        self.assertEqual(selector["algorithm"], "OcrDetect")
        self.assertEqual(selector["text"], [])
        self.assertEqual(selector["roi"], [20, 85, 250, 50])
        for key in ("fullMatch", "withoutDet", "useRaw", "binThreshold"):
            self.assertNotIn(key, selector)

    def test_overview_local_title_configuration(self):
        tasks = json.loads(
            (ROOT / "resource/tasks/InfrastPresetImport.json").read_text(
                encoding="utf-8"
            )
        )
        title = tasks["InfrastPresetOverviewLocalTitle"]
        self.assertEqual(title["algorithm"], "OcrDetect")
        self.assertEqual(title["text"], [])
        for key in ("fullMatch", "withoutDet", "useRaw"):
            self.assertTrue(title[key])
        self.assertEqual(title["binThreshold"], [0, 255])
        self.assertNotIn("baseTask", title)
        self.assertNotIn("roi", title)

    def test_local_overview_title_matrix(self):
        # Opt-in local screenshots, not a CI fixture or a claim of CI image coverage.
        # Set MAA_PRESET_OVERVIEW_MATRIX to the prior confirmation JSON; images sit beside it.
        fixture = os.environ.get("MAA_PRESET_OVERVIEW_MATRIX")
        if not fixture:
            self.skipTest(
                "Set MAA_PRESET_OVERVIEW_MATRIX for the local Core screenshot matrix"
            )
        path = pathlib.Path(fixture)
        matrix = json.loads(path.read_text(encoding="utf-8"))
        tasks = {}
        cases = []
        for index, row in enumerate(matrix["rows"]):
            x, y, _width, height = row["detected"]["rect"]
            roi = [x - 4, y - 4, 100, height + 8]
            self.assertEqual(roi, row["roi"])
            for target_index, target in enumerate(matrix["labels"]):
                previous = next(
                    result
                    for result in row["results"]
                    if result["task"].endswith(f"Id{target_index}")
                )
                name = f"PresetOverviewMatrix{index}Target{target_index}"
                tasks[name] = {
                    "baseTask": "InfrastPresetOverviewLocalTitle",
                    "roi": roi,
                    "text": [target],
                }
                cases.append((row, name, previous["hit"]))
        self.assertTrue(cases)
        with tempfile.TemporaryDirectory(prefix="maa-overview-matrix-") as directory:
            overlay = pathlib.Path(directory)
            task_dir = overlay / "resource/tasks"
            task_dir.mkdir(parents=True)
            (task_dir / "matrix.json").write_text(
                json.dumps(tasks, ensure_ascii=False), encoding="utf-8"
            )
            self.assertTrue(
                self.core._lib.AsstLoadResource(str(overlay).encode("utf-8"))
            )
            for image in sorted({row["image"] for row, _, _ in cases}):
                image_cases = [case for case in cases if case[0]["image"] == image]
                results = self.core.report(
                    [path.parent / image], [name for _, name, _ in image_cases]
                )[0]["results"]
                for (_, name, expected), result in zip(
                    image_cases, results, strict=True
                ):
                    with self.subTest(image=image, task=name):
                        self.assertEqual(result["task"], name)
                        self.assertEqual(result["hit"], expected)
        print(f"Local Core overview matrix: {len(cases)} checks; not CI coverage")

    def test_preset_init_mode_gates(self):
        self.assertEqual(self.append(self.config(), mode=0), 0)
        self.assertEqual(self.append(self.config(), mode=20000), 0)
        self.assertEqual(self.append(self.config(), preset_init="true"), 0)
        self.assertGreater(self.append(self.config()), 0)

    def test_ordinary_custom_unaffected(self):
        self.assertGreater(
            self.append(self.config(), preset_init=False, facility=["Mfg"]), 0
        )
        self.assertEqual(
            self.append(
                self.config(), preset_init=False, facility=["Mfg"], plan_index=9
            ),
            0,
        )
        # beta.2 起普通 Custom 对未知产物保留 Unknown 继续；预设导入仍严格拒绝
        unknown = self.config({"operators": ["清流"], "product": "invalid"})
        self.assertGreater(self.append(unknown, preset_init=False, facility=["Mfg"]), 0)
        self.assertEqual(self.append(unknown), 0)

    def test_valid_products_by_facility(self):
        cases = [
            (
                "manufacture",
                ["Battle Record", "Pure Gold", "Dualchip", "Originium Shard"],
            ),
            ("trading", ["LMD", "Orundum"]),
        ]
        for facility, products in cases:
            for product in products:
                with self.subTest(facility=facility, product=product):
                    room = {"operators": ["清流"], "product": product}
                    self.assertGreater(
                        self.append(self.config(room, facility=facility)), 0
                    )

    def test_invalid_product_facility_combos_rejected(self):
        cases = [
            ("manufacture", "LMD"),
            ("manufacture", "Orundum"),
            ("trading", "Battle Record"),
            ("trading", "Pure Gold"),
            ("power", "LMD"),
            ("control", "Pure Gold"),
        ]
        for facility, product in cases:
            with self.subTest(facility=facility, product=product):
                room = {"operators": ["清流"], "product": product}
                self.assertEqual(self.append(self.config(room, facility=facility)), 0)

    def test_room_product_unification(self):
        # 全部未指定：不做产物校验
        self.assertGreater(self.append(self.config({"operators": ["清流", "温蒂"]})), 0)
        # 同计划不同房间可各自指定不同产物
        rooms = {
            "manufacture": [
                {"operators": ["温蒂", "清流", "冬时"], "product": "Pure Gold"}
            ]
        }
        rooms["trading"] = [
            {"operators": ["但书", "能天使", "蕾缪安"], "product": "LMD"}
        ]
        self.assertGreater(self.append({"plans": [{"rooms": rooms}]}), 0)

        # 同房间跨计划：未指定沿用已指定的唯一产物；统一指定接受；冲突整份拒绝
        def plans(first, second):
            room = {"operators": ["清流", "温蒂"]}
            return {
                "plans": [
                    {
                        "rooms": {
                            "manufacture": [
                                dict(room, product=first) if first else dict(room)
                            ]
                        }
                    },
                    {
                        "rooms": {
                            "manufacture": [
                                dict(room, product=second) if second else dict(room)
                            ]
                        }
                    },
                ]
            }

        self.assertGreater(self.append(plans(None, "Pure Gold")), 0)
        self.assertGreater(self.append(plans("Pure Gold", "Pure Gold")), 0)
        self.assertEqual(self.append(plans("Pure Gold", "Battle Record")), 0)

    def test_invalid_inputs_rejected(self):
        cases = [
            {"plans": []},
            {"plans": [{}]},
            self.config(facility="dormitory"),
            self.config({"operators": ["清流"], "autofill": True}),
            self.config({"operators": ["清流"], "candidates": ["温蒂"]}),
            self.config({"operators": ["清流", "清流"]}),
        ]
        for config in cases:
            with self.subTest(config=config):
                self.assertEqual(self.append(config), 0)


if __name__ == "__main__":
    unittest.main()
