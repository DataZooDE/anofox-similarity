#!/usr/bin/env python3
"""
Download and extract Figshare Consumer Electronics dataset.

Dataset: Material composition of consumer electronics (Babbitt et al.)
DOI: 10.6084/m9.figshare.11306792.v4
License: CC0 (Public Domain)

Usage:
    python scripts/download_figshare.py
    python scripts/download_figshare.py --output test/data/figshare/raw
"""

import argparse
import hashlib
import io
import zipfile
from pathlib import Path
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError


FIGSHARE_URL = "https://ndownloader.figshare.com/articles/11306792/versions/4"
DEFAULT_OUTPUT = "test/data/figshare/raw"

# Expected files in the dataset (for validation)
EXPECTED_FILES = [
    "Disassembly Detail.xlsx",
    "Product Bill of Materials.xlsx",
    "Uncertainty Analysis.xlsx",
]


def download_with_progress(url: str, desc: str = "Downloading") -> bytes:
    """Download file with progress indication."""
    print(f"{desc}: {url}")

    headers = {"User-Agent": "anofox-similarity/1.0 (BOM similarity testing)"}
    request = Request(url, headers=headers)

    try:
        with urlopen(request, timeout=60) as response:
            total_size = response.headers.get("Content-Length")
            if total_size:
                total_size = int(total_size)
                print(f"  Size: {total_size / 1024 / 1024:.1f} MB")

            data = response.read()
            print(f"  Downloaded: {len(data) / 1024 / 1024:.1f} MB")
            return data

    except HTTPError as e:
        raise RuntimeError(f"HTTP Error {e.code}: {e.reason}") from e
    except URLError as e:
        raise RuntimeError(f"URL Error: {e.reason}") from e


def validate_zip(data: bytes) -> bool:
    """Validate that the downloaded file is a valid ZIP."""
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            names = zf.namelist()
            print(f"  ZIP contains {len(names)} files:")
            for name in names:
                print(f"    - {name}")
            return True
    except zipfile.BadZipFile:
        return False


def extract_zip(data: bytes, output_dir: Path) -> list[str]:
    """Extract ZIP contents to output directory."""
    output_dir.mkdir(parents=True, exist_ok=True)

    extracted = []
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        for name in zf.namelist():
            # Skip directories and hidden files
            if name.endswith("/") or name.startswith("__MACOSX"):
                continue

            # Extract file
            content = zf.read(name)
            # Use just the filename, not the full path
            filename = Path(name).name
            out_path = output_dir / filename
            out_path.write_bytes(content)
            extracted.append(filename)
            print(f"  Extracted: {filename} ({len(content) / 1024:.1f} KB)")

    return extracted


def validate_dataset(output_dir: Path, extracted: list[str]) -> bool:
    """Validate that expected files were extracted."""
    missing = []
    for expected in EXPECTED_FILES:
        if expected not in extracted:
            missing.append(expected)

    if missing:
        print(f"\nWarning: Missing expected files: {missing}")
        print("  Dataset structure may have changed.")
        return False

    return True


def write_metadata(output_dir: Path, extracted: list[str]) -> None:
    """Write metadata file with download info."""
    import json
    from datetime import datetime

    metadata = {
        "source": "figshare",
        "doi": "10.6084/m9.figshare.11306792.v4",
        "url": FIGSHARE_URL,
        "license": "CC0",
        "citation": "Babbitt, C. W., Althaf, S., Cruz Rios, F., & Bilec, M. M. (2020). Material composition of consumer electronics. figshare. Dataset.",
        "downloaded_at": datetime.utcnow().isoformat() + "Z",
        "files": extracted,
    }

    meta_path = output_dir / "download_metadata.json"
    with open(meta_path, "w") as f:
        json.dump(metadata, f, indent=2)
    print(f"\nMetadata written to: {meta_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Download Figshare Consumer Electronics dataset"
    )
    parser.add_argument(
        "--output",
        type=str,
        default=DEFAULT_OUTPUT,
        help=f"Output directory (default: {DEFAULT_OUTPUT})",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force re-download even if files exist",
    )
    args = parser.parse_args()

    output_dir = Path(args.output)

    # Check if already downloaded
    if not args.force and (output_dir / "download_metadata.json").exists():
        print(f"Dataset already downloaded to: {output_dir}")
        print("Use --force to re-download")
        return

    print("=" * 60)
    print("Figshare Consumer Electronics Dataset Download")
    print("=" * 60)
    print(f"DOI: 10.6084/m9.figshare.11306792.v4")
    print(f"License: CC0 (Public Domain)")
    print()

    # Download
    data = download_with_progress(FIGSHARE_URL, "Downloading dataset")

    # Validate ZIP
    print("\nValidating ZIP archive...")
    if not validate_zip(data):
        raise RuntimeError("Downloaded file is not a valid ZIP archive")

    # Extract
    print(f"\nExtracting to: {output_dir}")
    extracted = extract_zip(data, output_dir)

    # Validate contents
    print("\nValidating dataset contents...")
    validate_dataset(output_dir, extracted)

    # Write metadata
    write_metadata(output_dir, extracted)

    print("\n" + "=" * 60)
    print("Download complete!")
    print(f"Next step: python scripts/prepare_figshare_data.py")
    print("=" * 60)


if __name__ == "__main__":
    main()
