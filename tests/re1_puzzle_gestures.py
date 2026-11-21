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

    def jump(room):
        game.ask(f"ini-set Play imported games/Testing/Room {room}")
        action("Go to that room")
        actual = state()
        check(actual["Status"].startswith(f"Room 1-{room:02X}"), actual)

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

    def use(item, slot, pose):
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
        for _ in range(12):
            game.adv(16)
            if log().count("puzzle gesture: finished") > ends:
                break
        check(log().count("puzzle gesture: finished") == ends + 1, f"{item}: gesture did not finish")
        if pose == "piano":
            shots = [line.removeprefix(camera_prefix) for line in log()[len(initial_log):].splitlines()
                     if line.startswith(camera_prefix)]
            check(shots == ["6", "1", "7", "1"], f"Piano event camera sequence: {shots}")
        actual = state()
        check(item not in actual["Pocket"], actual)
        game.adv(20)
        game.press(CROSS, 55)
        print(f"{item}: gesture completed, item used and dialogue dismissed", flush=True)

    game.ask("ini-set Play imported games/Scenario 1")
    action("Start", False)
    jump(5)
    pickup("Wood Emblem")
    jump(15)
    pickup("Music Notes", 4)
    use("Music Notes", 1, "piano")
    use("Wood Emblem", 0, "reach")
    pickup("Gold Emblem", 4)
    jump(5)
    use("Gold Emblem", 0, "reach")
    pickup("Shield Key", 3)


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
