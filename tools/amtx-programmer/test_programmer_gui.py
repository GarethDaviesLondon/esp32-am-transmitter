"""The Program tab driven against the simulated board, with the window hidden.

Skipped where Tk cannot open a window (no display).
"""

import subprocess
import sys
import time
from pathlib import Path

import pytest

import amtx_console as ac
from fake_board import FakeBoard


@pytest.fixture
def app():
    tk = pytest.importorskip("tkinter")
    try:
        root = tk.Tk()
    except tk.TclError as exc:
        pytest.skip("no display: {}".format(exc))
    import amtx_programmer
    root.withdraw()
    board = FakeBoard(smart=True, reboot_seconds=0.4)
    application = amtx_programmer.ProgrammerApp(root, sim_board=board)
    yield root, application, board
    application.on_close()


def pump_until(root, condition, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        root.update()
        if condition():
            return True
        time.sleep(0.02)
    return False


def test_connect_act_reboot_and_reconnect(app, monkeypatch):
    root, application, board = app
    program = application.program
    program.connect()
    assert pump_until(root, lambda: program.console.connected and program.doc is not None)
    assert len(program.station_tree.get_children()) == 2
    assert program.host_var.get().startswith("amtx")
    assert program.rf_vars[1]["hz"].get() == "250000"

    # An action goes to the board, shows its reply, and refreshes the view.
    program.act("Add", lambda b: b.station_add("Test FM", "http://test/fm"))
    assert pump_until(root, lambda: len(program.station_tree.get_children()) == 3)
    assert "ok" in program.status_var.get() or "Refresh" in program.status_var.get()

    # A failing action is reported, never silent.
    warnings = []
    monkeypatch.setattr(ac, "DEFAULT_TIMEOUT", ac.DEFAULT_TIMEOUT)
    import amtx_programmer
    monkeypatch.setattr(amtx_programmer.messagebox, "showwarning", lambda *a, **k: warnings.append(a))
    program.act("Delete 42", lambda b: b.station_delete(42))
    assert pump_until(root, lambda: warnings)
    assert "ESP_ERR_INVALID_ARG" in warnings[0][1]

    # The form sends every rf key.
    program.rf_vars[0]["depth"].set("55")
    program.rf_vars[0]["fade"].set("slow")
    program.apply_rf(0)
    assert pump_until(root, lambda: board.nvs["rf"][0]["depth"] == 55 and board.nvs["rf"][0]["fade"] == 1)

    # Reboot: the tab reconnects by itself when the board comes back.
    program.submit("Reboot", lambda b: b.reboot())
    assert pump_until(root, lambda: board.boots == 1 and not program.console.connected, timeout=5)
    assert pump_until(root, lambda: program.console.connected, timeout=10)
    board.nvs["host"] = "after-reboot"
    program.refresh(explicit=False)
    assert pump_until(root, lambda: program.host_var.get().startswith("after-reboot"))


def test_check_mode_exits_cleanly():
    # In its own interpreter: a second Tk in one process, with the first app's
    # threads still winding down, intermittently fails to find its Tcl library.
    script = Path(__file__).resolve().parent / "amtx_programmer.py"
    result = subprocess.run([sys.executable, str(script), "--check"], capture_output=True, text=True, timeout=60)
    if result.returncode != 0 and "TclError" in result.stderr:
        pytest.skip("no display: {}".format(result.stderr.strip().splitlines()[-1]))
    assert result.returncode == 0, result.stdout + result.stderr
    assert "simulated board connected" in result.stdout


def test_without_answering_cursor_queries_a_command_is_swallowed():
    """Why the reader answers ESC[6n: the fake, like linenoise, eats input as the answer otherwise."""
    board = FakeBoard(smart=True, noisy=False)
    transport = board.open()
    transport.read(0.1)                   # the prompt and its queries arrive; nobody answers
    transport.write(b"state\r")
    assert any(c.startswith("<swallowed") for c in board.commands)
    assert "state" not in board.commands
