# The repo runs pytest in importlib mode, which does not put a test's directory
# on sys.path. The tests here import their neighbours by name, so add it.
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
