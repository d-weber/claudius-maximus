import argparse
import logging

from .daemon import run


def main():
    ap = argparse.ArgumentParser(prog="companion", description="Claudius Maximus host daemon")
    ap.add_argument("--demo", action="store_true", help="stream fake data instead of real sessions")
    ap.add_argument("--port", help="serial port (auto-detected by default)")
    ap.add_argument("--dry-run", action="store_true", help="print JSON to stdout instead of the device")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    run(demo=a.demo, port=a.port, dry_run=a.dry_run)


if __name__ == "__main__":
    main()
