#!/usr/bin/env python3
"""
Proiect curs - T32 sound sequencer - client ordinar alternativ
"""

import argparse
import os


def main() -> int:
    parser = argparse.ArgumentParser(description="client ordinar alternativ")
    parser.add_argument("--host", default="127.0.0.1", help="hostul planificat pentru server")
    parser.add_argument("--port", default="19090", help="portul INET planificat")
    parser.add_argument("--composition", default="demo_string_layers", help="numele compozitiei")
    parser.add_argument("--show-request", action="store_true", help="afiseaza cererea demo")
    args = parser.parse_args()

    print("client ordinar alternativ")
    print(f"destinatie planificata: {args.host}:{args.port}")
    print(f"compozitie: {args.composition}")
    print(f"USER={os.environ.get('USER', 'nedefinit')}")

    if args.show_request:
        print("cerere demo:")
        print(f"UPLOAD {args.composition} 4096")
        print("CHUNK 0 1024")
        print(f"STATUS {args.composition}")
        print(f"RESULT {args.composition}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())