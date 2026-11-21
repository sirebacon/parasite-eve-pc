"""Run the RE1 Shield Key interactions through the original game's controller harness."""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from enemy_import_fights import CROSS, Failed, Game, check

SQUARE = 32768
RIGHT = 32


def test_puzzles(game):
    def state():
        rows = json.loads(game.ask("mod-state pe_re_play"))["rows"]
        return {row["label"]: row["value"] for row in rows}

    def action(name, testing=True):
        section = "Testing/" if testing else ""
        return game.ask(f"mod-action Play imported games/{section}{name}")

    def jump(room, stage=1):
        game.ask(f"ini-set Play imported games/Testing/Stage {stage}")
        game.ask(f"ini-set Play imported games/Testing/Room {room}")
        action("Go to that room")
        actual = state()
        check(actual["Status"].startswith(f"Room {stage}-{room:02X}"), actual)

    def pickup(item, attempts=1):
        for _ in range(attempts):
            action("Go to the next pickup")
            game.press(CROSS, 55)
            actual = state()
            if item in actual["Pocket"]:
                game.press(CROSS, 55)
                return
        raise Failed(f"Could not pick up {item}: {actual}")

    def log():
        return (game.config / "pe_mods.log").read_text(encoding="utf-8", errors="replace")

    def use(item, slot, pose, retained=False):
        action("Go to the next place to use an item")
        game.adv(10)
        if pose == "piano":
            game.raw("capture idle-piano")
        initial_log = log()
        starts = initial_log.count(f"puzzle gesture: {pose}")
        ends = initial_log.count("puzzle gesture: finished")
        camera_prefix = "pe_re_play: puzzle camera: "
        game.press(SQUARE, 40)
        for _ in range(slot):
            game.press(RIGHT, 10)
        game.adv(10, CROSS)
        game.adv(4)
        check(log().count(f"puzzle gesture: {pose}") == starts + 1, f"{item}: gesture did not start")
        check(log().count("puzzle gesture: finished") == ends, f"{item}: gesture ended before capture")
        if pose == "piano":
            for _ in range(6):
                if camera_prefix + "6" in log()[len(initial_log):]:
                    break
                game.adv(4)
            check(camera_prefix + "6" in log()[len(initial_log):], "Piano event's first camera did not load")
            game.raw("capture gesture-piano")
            idle = game.work / "idle-piano.ppm"
            gesture = game.work / "gesture-piano.ppm"
            check(idle.read_bytes() != gesture.read_bytes(), "Piano pose did not change the rendered frame")
            for _ in range(30):
                if camera_prefix + "7" in log()[len(initial_log):]:
                    break
                game.adv(3)
            shots = [line for line in log()[len(initial_log):].splitlines() if line.startswith(camera_prefix)]
            check(shots[-1:] == [camera_prefix + "7"], "Alcove camera did not load")
            game.adv(4)
            game.raw("capture panel-low")
            game.adv(6)
            updated_shots = [line for line in log()[len(initial_log):].splitlines()
                             if line.startswith(camera_prefix)]
            check(updated_shots == shots, "Camera changed during the panel comparison")
            game.raw("capture panel-rising")
            low = (game.work / "panel-low.ppm").read_bytes()
            rising = (game.work / "panel-rising.ppm").read_bytes()
            changed = sum(a != b for a, b in zip(low, rising))
            check(1000 < changed < len(low) // 4,
                  "The panel's camera did not show a visible change")
        if item == "Gold Emblem":
            for _ in range(10):
                if "clock camera: 5" in log()[len(initial_log):]:
                    break
                game.adv(2)
            check("clock camera: 5" in log()[len(initial_log):], "Dining Room clock camera did not load")
            game.adv(4)
            game.raw("capture clock-start")
            game.adv(14)
            game.raw("capture clock-sliding")
            start = (game.work / "clock-start.ppm").read_bytes()
            sliding = (game.work / "clock-sliding.ppm").read_bytes()
            changed = sum(a != b for a, b in zip(start, sliding))
            check(1000 < changed < len(start) // 4, f"The clock close-up changed {changed} color bytes")
        if item == "Square Crank":
            for _ in range(12):
                if "pool camera: 2" in log()[len(initial_log):]:
                    break
                game.adv(2)
            check("pool camera: 2" in log()[len(initial_log):], "Pool camera did not load")
            game.adv(4)
            game.raw("capture pool-full")
            game.adv(30)
            check(log().count("puzzle gesture: finished") == ends, "Pool camera returned before capture")
            game.raw("capture pool-drained")
            full = (game.work / "pool-full.ppm").read_bytes()
            drained = (game.work / "pool-drained.ppm").read_bytes()
            changed = sum(a != b for a, b in zip(full, drained))
            check(1000 < changed < len(full) // 2, f"The pool view changed {changed} color bytes")
        if item == "Red Book":
            for _ in range(16):
                if "bookshelf camera: 5" in log()[len(initial_log):]:
                    break
                game.adv(2)
            check("bookshelf camera: 5" in log()[len(initial_log):], "Bookshelf camera did not load")
            game.adv(4)
            game.raw("capture bookshelf-start")
            game.adv(30)
            check("bookshelf camera: 2" not in log()[len(initial_log):], "Bookshelf camera changed before capture")
            game.raw("capture bookshelf-sliding")
            start = (game.work / "bookshelf-start.ppm").read_bytes()
            sliding = (game.work / "bookshelf-sliding.ppm").read_bytes()
            changed = sum(a != b for a, b in zip(start, sliding))
            check(1000 < changed < len(start) // 4, f"The bookshelf view changed {changed} color bytes")
        for _ in range(24):
            game.adv(16)
            if log().count("puzzle gesture: finished") > ends:
                break
        check(log().count("puzzle gesture: finished") == ends + 1, f"{item}: gesture did not finish")
        if pose == "piano":
            shots = [line.removeprefix(camera_prefix) for line in log()[len(initial_log):].splitlines()
                     if line.startswith(camera_prefix)]
            check(shots == ["6", "1", "7", "1"], f"Piano event camera sequence: {shots}")
        if item == "Red Book":
            shots = [line.rsplit(" ", 1)[-1] for line in log()[len(initial_log):].splitlines()
                     if "bookshelf camera: " in line]
            check(shots == ["5", "2", "3"], f"Bookshelf event camera sequence: {shots}")
        actual = state()
        check((item in actual["Pocket"]) == retained, actual)
        game.adv(20)
        game.press(CROSS, 55)
        print(f"{item}: gesture completed and dialogue dismissed", flush=True)

    game.ask("ini-set Play imported games/Scenario 1")
    action("Start", False)
    jump(5)
    pickup("Wood Emblem")
    jump(15)
    check("room prop: loaded re1:object/10f-0" in log(), "Imported alcove panel did not load")
    pickup("Music Notes", 4)
    use("Music Notes", 1, "piano")
    use("Wood Emblem", 0, "reach")
    pickup("Gold Emblem", 4)
    jump(5)
    check("room prop: loaded re1:object/105-2" in log(), "Imported Dining Room clock did not load")
    use("Gold Emblem", 0, "reach")
    pickup("Shield Key", 3)
    jump(0x1B)
    pickup("Square Crank", 3)
    jump(1, stage=3)
    check("room prop: loaded re1:object/301-1" in log(), "Imported pool water did not load")
    use("Square Crank", state()["Pocket"].split(", ").index("Square Crank"), "reach", retained=True)

    for label, floor in (("descending", 1), ("ascending", 0)):
        action("Go to the next ramp")
        start = len(log())
        game.adv(10, CROSS)
        check(f"pool ladder: {label}" in log()[start:], state())
        game.raw(f"capture ladder-{label}-start")
        game.adv(17)
        game.raw(f"capture ladder-{label}-moving")
        still = (game.work / f"ladder-{label}-start.ppm").read_bytes()
        moving = (game.work / f"ladder-{label}-moving.ppm").read_bytes()
        changed = sum(a != b for a, b in zip(still, moving))
        check(400 < changed < len(still) // 6, f"The ladder view changed {changed} color bytes")
        for _ in range(8):
            if "pool ladder: finished" in log()[start:]:
                break
            game.adv(8)
        actual = state()
        check("pool ladder: finished" in log()[start:] and f"floor {floor}" in actual["Status"], actual)

    jump(1, stage=4)
    pickup("Red Book")
    jump(0x0A, stage=4)
    check("room prop: loaded re1:object/40a-0" in log(), "Imported bookshelf did not load")
    use("Red Book", state()["Pocket"].split(", ").index("Red Book"), "reach")


def main():
    disc = os.getenv("PE_TEST_DISC")
    re1_disc = os.getenv("PE_TEST_RE1_DISC")
    if not disc or not re1_disc or not Path(disc).is_file() or not Path(re1_disc).is_file():
        print("PE_TEST_DISC and PE_TEST_RE1_DISC are required: skipped")
        return 77
    exe, session, work = (Path(arg).resolve() for arg in sys.argv[1:])
    import_work = work / "import"
    import_work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PE_TEST_RE1_DISC=str(Path(re1_disc).resolve()))
    env.pop("PE_TEST_RE2_DISC", None)
    subprocess.run([str(session)], cwd=import_work, env=env, check=True)
    store = import_work / "re1-jill-session-test" / "store"

    def setup(config):
        shutil.copytree(store, config / "import" / "store")

    game = Game(exe, str(Path(disc).resolve()), work / "game", setup=setup,
                ini="Manage mods/Play imported games=1\n")
    try:
        test_puzzles(game)
    except Failed as error:
        print(f"FAILED: {error}")
        return 1
    finally:
        game.quit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
