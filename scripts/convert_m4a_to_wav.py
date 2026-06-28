#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert an .m4a audio file to .wav using ffmpeg."
    )
    parser.add_argument("input_file", type=Path, help="Path to the source .m4a file")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Path for the output .wav file. Defaults to the input filename with a .wav extension.",
    )
    return parser


def resolve_output_path(input_file: Path, output_file: Path | None) -> Path:
    if output_file is not None:
        return output_file
    return input_file.with_suffix(".wav")


def find_ffmpeg() -> str | None:
    ffmpeg_path = shutil.which("ffmpeg")
    if ffmpeg_path is not None:
        return ffmpeg_path

    if sys.platform == "darwin":
        # VS Code/Python can run with a reduced PATH on macOS.
        for candidate in (
            Path("/opt/homebrew/bin/ffmpeg"),
            Path("/usr/local/bin/ffmpeg"),
        ):
            if candidate.is_file():
                return str(candidate)

    return None


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    input_file = args.input_file.expanduser().resolve()
    output_file = resolve_output_path(input_file, args.output)

    if not input_file.is_file():
        parser.error(f"Input file does not exist: {input_file}")

    ffmpeg_path = find_ffmpeg()
    if ffmpeg_path is None:
        print(
            "Error: ffmpeg is not installed or not on PATH. On macOS, install with 'brew install ffmpeg'.",
            file=sys.stderr,
        )
        return 1

    output_file.parent.mkdir(parents=True, exist_ok=True)

    command = [
        ffmpeg_path,
        "-y",
        "-i",
        str(input_file),
        "-vn",
        "-acodec",
        "pcm_s16le",
        str(output_file),
    ]

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        if result.stderr:
            print(result.stderr.strip(), file=sys.stderr)
        return result.returncode

    print(f"Created: {output_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())