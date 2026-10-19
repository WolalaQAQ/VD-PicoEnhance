"""Run the production UI classes against host doubles; no VD/device required."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
DOTNET = (str(Path(os.environ['DOTNET_ROOT']) / ('dotnet.exe' if os.name == 'nt' else 'dotnet'))
          if os.environ.get('DOTNET_ROOT') else shutil.which('dotnet'))


@unittest.skipUnless(DOTNET, 'dotnet SDK required for managed UI tests')
class ManagedUiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(tempfile.gettempdir()) / 'opencode'
        parent.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='vd-managed-ui-', dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        output = Path(cls.temp.name)
        version = subprocess.check_output([DOTNET, '--version'], text=True).strip()
        framework = 'net' + version.split('.')[0] + '.0'
        cls.run_command('build', ROOT / 'tools/checks/managed-ui/ManagedUi.csproj', '-v', 'q',
                        f'-p:HarnessFramework={framework}',
                        f'-p:BaseIntermediateOutputPath={output / "obj"}/',
                        f'-p:OutputPath={output / "bin"}/')
        cls.assembly = output / 'bin/ManagedUi.dll'
        shutil.copy2(ROOT / 'zygisk/payload/ui_zh.txt', cls.assembly.parent / 'ui_zh.txt')

    @staticmethod
    def run_command(*args):
        result = subprocess.run([DOTNET, *(str(arg) for arg in args)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout

    def test_switch_needs_no_external_enable_setting(self):
        self.run_command(self.assembly, 'legacy-disabled')
        self.run_command(self.assembly, 'unset')
        source = (ROOT / 'mod/src/VdHsMod/GestureToggle.cs').read_text(encoding='utf-8')
        self.assertNotIn('VDHS_SYSTEM_GESTURES_ENABLED', source)

    def test_first_display_is_off_without_creating_a_setting(self):
        self.run_command(self.assembly, 'default-off')

    def test_explicit_off_survives_restart(self):
        self.run_command(self.assembly, 'saved-off')

    def test_explicit_on_survives_restart(self):
        self.run_command(self.assembly, 'saved-on')

    def test_invalid_saved_value_defaults_to_off(self):
        self.run_command(self.assembly, 'invalid')

    def test_saved_override_parser_matches_service_mirror(self):
        for scenario in ('spaced-off', 'spaced-on', 'crlf-off'):
            self.run_command(self.assembly, scenario)
        source = (ROOT / 'mod/src/VdHsMod/GestureToggle.cs').read_text(encoding='utf-8')
        service = (ROOT / 'zygisk/service.sh').read_text(encoding='utf-8')
        self.assertIn('File.ReadAllText(path).Replace("\\r", "").Replace("\\n", "")', source)
        self.assertIn("tr -d '\\r\\n'", service)

    def test_user_can_switch_off_and_on(self):
        self.run_command(self.assembly, 'roundtrip')

    def test_gesture_switch_has_chinese_caption_and_font(self):
        self.run_command(self.assembly, 'toggle-zh')

    def test_gesture_switch_keeps_english_caption_and_font_on_english_system(self):
        self.run_command(self.assembly, 'toggle-en')

    def test_localization_preserves_dynamic_subtrees_and_values(self):
        self.run_command(self.assembly, 'localization')

    def test_localization_caches_targets_without_repeated_redraw(self):
        self.run_command(self.assembly, 'cache')

    def test_localization_waits_for_cjk_font(self):
        self.run_command(self.assembly, 'font-late')

    def test_late_gesture_toggle_is_translated_without_rescanning_page(self):
        self.run_command(self.assembly, 'new-toggle')

    def test_recreated_ui_refreshes_bindings_and_font(self):
        self.run_command(self.assembly, 'new-root')

    def test_caption_change_uses_event_without_scanning(self):
        self.run_command(self.assembly, 'new-tab')

    def test_missing_dispatcher_does_not_modify_ui_off_thread(self):
        self.run_command(self.assembly, 'no-dispatcher')

    def test_dictionary_is_plain_two_column_text(self):
        for line in (ROOT / 'zygisk/payload/ui_zh.txt').read_text(encoding='utf-8').splitlines():
            if line and not line.startswith('#'):
                self.assertEqual(len(line.split('\t')), 2, line)

    def test_waits_for_cached_page(self):
        self.run_command(self.assembly, 'resource-late')

    def test_rejects_nonshared_page_before_mutation(self):
        self.run_command(self.assembly, 'resource-alias')

    def test_waits_for_all_seven_tabs_to_finish_loading(self):
        self.run_command(self.assembly, 'incomplete-page')

    def test_templates_inherit_text_and_font_without_listeners_or_data_mutation(self):
        self.run_command(self.assembly, 'templates')

    def test_off_thread_caption_event_queues_ui_work(self):
        self.run_command(self.assembly, 'off-thread')

    def test_disposal_releases_listeners_and_invalidates_pending_changes(self):
        self.run_command(self.assembly, 'pending-dispose')

    def test_unknown_caption_restores_font_once_without_changing_text(self):
        self.run_command(self.assembly, 'unknown-text')

    def test_multiline_dictionary_decodes_explicit_line_breaks(self):
        self.run_command(self.assembly, 'table')

    def test_system_language_automatically_enables_only_chinese(self):
        self.run_command(self.assembly, 'language')
        source = (ROOT / 'mod/src/VdHsMod/UiLocalize.cs').read_text(encoding='utf-8')
        self.assertIn('LoadTable(CultureInfo.InstalledUICulture.Name)', source)

    def test_ui_caption_finishes_before_setter_returns_without_queueing(self):
        self.run_command(self.assembly, 'event')

    def test_all_quality_presets_finish_translation_before_render(self):
        self.run_command(self.assembly, 'quality-cycle')

    def test_pending_async_work_cannot_replace_a_newer_ui_selection(self):
        self.run_command(self.assembly, 'latest-caption')

    def test_repeated_ui_caption_skips_full_redraw_and_font_assignment(self):
        self.run_command(self.assembly, 'repeat-caption')

    def test_repeated_async_caption_still_clears_potentially_rendered_english(self):
        self.run_command(self.assembly, 'repeat-async')

    def test_repeated_caption_redraws_if_host_changed_the_font(self):
        self.run_command(self.assembly, 'repeat-font')

    def test_independent_overlay_translates_states_without_touching_telemetry(self):
        self.run_command(self.assembly, 'overlay')

    def test_overlay_can_finish_loading_after_the_control_panel(self):
        self.run_command(self.assembly, 'overlay-late')

    def test_waits_for_the_cached_overlay_page(self):
        self.run_command(self.assembly, 'overlay-page-late')

    def test_rejects_nonshared_overlay_before_mutation(self):
        self.run_command(self.assembly, 'overlay-alias')

    def test_overlay_recreation_releases_and_rebinds_listeners(self):
        self.run_command(self.assembly, 'overlay-reload')

    def test_pending_overlay_callback_cannot_touch_disposed_ui(self):
        self.run_command(self.assembly, 'overlay-pending-dispose')

    def test_mixed_statuses_restore_original_font_for_numbers_and_preserved_names(self):
        self.run_command(self.assembly, 'font-restore')

    def test_queued_callbacks_do_not_restore_font_for_our_own_translation(self):
        self.run_command(self.assembly, 'font-pending')

    def test_event_registration_uses_vd_retained_reflection_accessors(self):
        source = (ROOT / 'mod/src/VdHsMod/UiLocalize.cs').read_text(encoding='utf-8')
        self.assertNotIn('.AddEventHandler(', source)
        self.assertNotIn('.RemoveEventHandler(', source)
        self.assertNotIn('EventInfo', source)
        self.assertNotIn('.GetEvent(', source)
        self.assertIn('.GetMethod("add_TextChanged")', source)
        self.assertIn('.GetMethod("remove_TextChanged")', source)

    def test_gesture_row_survives_panel_recreation(self):
        self.run_command(self.assembly, 'toggle-reload')

    def test_injector_has_no_external_gesture_setting_or_environment_gate(self):
        source = (ROOT / 'zygisk/jni/module.cpp').read_text(encoding='utf-8')
        self.assertNotIn('VDHS_SYSTEM_GESTURES_ENABLED', source)
        self.assertNotIn('payload/system_gestures.txt', source)


class LocalizationTerminologyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pairs = [line.split('\t') for line in
                     (ROOT / 'zygisk/payload/ui_zh.txt').read_text(encoding='utf-8').splitlines()
                     if line and not line.startswith('#')]
        cls.table = dict(cls.pairs)

    def test_dictionary_has_unique_nonempty_pairs(self):
        self.assertEqual(len(self.table), len(self.pairs), 'duplicate English caption')
        for source, target in self.pairs:
            self.assertTrue(source.strip())
            self.assertTrue(target.strip())
            self.assertEqual(source.count(r'\n'), target.count(r'\n'), source)

    def test_proprietary_technology_names_stay_intact(self):
        for name in ('Snapdragon® Game Super Resolution', 'Synchronous Spacewarp (SSW)'):
            self.assertEqual(self.table[name], name)

    def test_system_gesture_switch_is_bilingual(self):
        self.assertEqual(self.table['Suppress system gestures'], '屏蔽系统手势')
        source = (ROOT / 'mod/src/VdHsMod/GestureToggle.cs').read_text(encoding='utf-8')
        self.assertIn('private const string Label = "Suppress system gestures";', source)

    def test_brand_names_and_acronyms_survive_translation(self):
        for source, target in self.pairs:
            for name in ('Virtual Desktop', 'SteamVR', 'Oculus', 'Steam', 'Windows',
                         'Mac', 'Quest', 'Link', 'SGSR', 'Index', 'Streamer'):
                if name in source:
                    self.assertIn(name, target, source)
        self.assertIn('PICO', self.table['     Pico Username: '])
        self.assertIn('VIVE', self.table['Emulate SteamVR Vive trackers'])

    def test_quality_presets_preserve_joke_names_and_frame_rate_units(self):
        self.assertEqual([self.table[key] for key in
                          ('Potato', 'Low', 'Medium', 'High', 'Ultra', 'Godlike')],
                         ['Potato', '低', '中', '高', '超高', 'Godlike'])
        for rate in (60, 72, 80, 90, 96, 100, 120, 144):
            self.assertEqual(self.table[f'{rate} fps'], f'{rate} 帧/秒')

    def test_exact_performance_headings_and_states_preserve_units_and_markers(self):
        expected = {
            'Framerate': '帧率', 'Latency': '延迟', 'Bitrate': '码率', 'Max bitrate': '最大码率',
            'Codec': '编码格式', 'Runtime': '运行时', 'Headset': '头显', 'Game': '游戏',
            'Encoding': '编码', 'Networking': '网络', 'Decoding': '解码', 'Gfx Quality': '画质档位',
            'Render resolution': '渲染分辨率', 'Target framerate': '目标帧率',
            'Video buffering': '视频缓冲', 'Automatic bitrate': '自动码率',
            'PC Ethernet': '电脑有线网络', 'Spacewarp': 'Spacewarp',
            'No': '否', 'USB Speed': 'USB 速率', 'Not Gigabit': '非千兆',
            'Always': '始终启用', 'Active': '生效中', 'N/A': '不适用',
        }
        for source, target in expected.items():
            self.assertEqual(self.table[source], target)
        for source in ('Low', 'Medium', 'High', 'Ultra'):
            self.assertEqual(self.table[source + "'"], self.table[source] + "'")
        for unit in ('fps', 'ms', '%', 'Mbps', 'GHz', '2.0', '3.0'):
            self.assertNotIn(unit, self.table)

    def test_passthrough_portals_and_grip_use_feature_context(self):
        for key in ('Desk Portal', 'Create Desk Portal', 'Close Desk Portal'):
            self.assertIn('书桌透视窗口', self.table[key])
            self.assertNotIn('传送门', self.table[key])
        self.assertIn('正在', self.table['Drawing Portal'])
        self.assertIn('握持键', self.table['When right grip is pressed'])
        self.assertIn('触摸', self.table['When right thumbrest is active'])
        self.assertIn('预测生成', self.table[
            'Causes PC to switch to half framerate while headset extrapolates the missing frames'])

    def test_descriptions_keep_feature_limits_and_instruction_fragments(self):
        for source, target in self.pairs:
            if source.startswith('Blend VR games') or source.startswith('Upscales the image'):
                if '90fps' in source:
                    self.assertIn('90 帧/秒', target)
                if 'High quality' in source:
                    self.assertIn('High（高）档', target)
                    self.assertIn('串流画质最高支持', target)
                if 'disabled' in source:
                    self.assertIn('关闭', target)
                if 'Foveated streaming' in source:
                    self.assertIn(self.table['Foveated streaming'], target)
        sgsr = self.table['Upscales the image to increase detail. Usable up to High quality at 90fps']
        self.assertIn('串流画质最高支持 High（高）档', sgsr)
        self.assertIn('使用 High 档时，刷新率不得超过 90 帧/秒', sgsr)
        self.assertNotIn('90 帧/秒时最高支持高画质', sgsr)
        self.assertTrue(self.table['1.  Download and Install the Streamer app from '].endswith(' '))
        self.assertEqual(self.table['Recently played'], '最近播放')  # Video sort order, not a game label.


if __name__ == '__main__':
    unittest.main()
