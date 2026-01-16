#!/usr/bin/env python3
"""
Download BOM demo data from open source and commercial ERPs.

Usage:
    python download_erp_bom_data.py --output test/data/
    python download_erp_bom_data.py --commercial-only  # Just AdventureWorks, PDXpert, Neo4j
"""

import argparse
import io
import json
import urllib.request
import urllib.error
import zipfile
from pathlib import Path
from datetime import datetime


# Odoo 18.0 demo data URLs
ODOO_FILES = {
    "mrp_demo.xml": "https://raw.githubusercontent.com/odoo/odoo/18.0/addons/mrp/data/mrp_demo.xml",
    "mrp_data.xml": "https://raw.githubusercontent.com/odoo/odoo/18.0/addons/mrp/data/mrp_data.xml",
    "product_demo.xml": "https://raw.githubusercontent.com/odoo/odoo/18.0/addons/product/data/product_demo.xml",
}

# ERPNext demo data URLs
ERPNEXT_FILES = {
    "bom_test_records.json": "https://raw.githubusercontent.com/frappe/erpnext/develop/erpnext/manufacturing/doctype/bom/test_records.json",
    "item_test_records.json": "https://raw.githubusercontent.com/frappe/erpnext/develop/erpnext/stock/doctype/item/test_records.json",
}

# Microsoft AdventureWorks (SQL Server Samples)
ADVENTUREWORKS_FILES = {
    "BillOfMaterials.csv": "https://raw.githubusercontent.com/Microsoft/sql-server-samples/master/samples/databases/adventure-works/oltp-install-script/BillOfMaterials.csv",
    "Product.csv": "https://raw.githubusercontent.com/Microsoft/sql-server-samples/master/samples/databases/adventure-works/oltp-install-script/Product.csv",
}

# PDXpert PLM Sample (ZIP download)
PDXPERT_ZIP_URL = "https://www.buyplm.com/resources/Example-PDXpertImport.zip"

# Neo4j BOM Gist
NEO4J_FILES = {
    "bom.cypher": "https://gist.githubusercontent.com/maxdemarzi/e77145f0a77b7b5f6c9287bc0a96928f/raw/bom.cypher",
    "bom2.cypher": "https://gist.githubusercontent.com/maxdemarzi/e77145f0a77b7b5f6c9287bc0a96928f/raw/bom2.cypher",
}


def download_file(url: str, dest_path: Path) -> bool:
    """Download a file from URL to destination path."""
    try:
        print(f"  Downloading: {url}")
        with urllib.request.urlopen(url, timeout=30) as response:
            content = response.read()
            dest_path.write_bytes(content)
            print(f"  -> Saved to: {dest_path} ({len(content):,} bytes)")
            return True
    except urllib.error.HTTPError as e:
        print(f"  ERROR: HTTP {e.code} - {e.reason}")
        return False
    except urllib.error.URLError as e:
        print(f"  ERROR: {e.reason}")
        return False
    except Exception as e:
        print(f"  ERROR: {e}")
        return False


def download_odoo(output_dir: Path) -> dict:
    """Download Odoo MRP demo data."""
    print("\n=== Downloading Odoo MRP Demo Data ===")

    raw_dir = output_dir / "odoo" / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    results = {"downloaded": [], "failed": []}

    for filename, url in ODOO_FILES.items():
        dest = raw_dir / filename
        if download_file(url, dest):
            results["downloaded"].append(filename)
        else:
            results["failed"].append(filename)

    # Save download metadata
    metadata = {
        "source": "odoo",
        "version": "18.0",
        "repository": "https://github.com/odoo/odoo",
        "download_date": datetime.now().isoformat(),
        "files": results["downloaded"],
        "failed": results["failed"],
    }

    metadata_path = raw_dir / "download_metadata.json"
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)

    return results


def download_erpnext(output_dir: Path) -> dict:
    """Download ERPNext test records."""
    print("\n=== Downloading ERPNext Test Records ===")

    raw_dir = output_dir / "erpnext" / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    results = {"downloaded": [], "failed": []}

    for filename, url in ERPNEXT_FILES.items():
        dest = raw_dir / filename
        if download_file(url, dest):
            results["downloaded"].append(filename)
        else:
            results["failed"].append(filename)

    # Save download metadata
    metadata = {
        "source": "erpnext",
        "branch": "develop",
        "repository": "https://github.com/frappe/erpnext",
        "download_date": datetime.now().isoformat(),
        "files": results["downloaded"],
        "failed": results["failed"],
    }

    metadata_path = raw_dir / "download_metadata.json"
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)

    return results


def download_adventureworks(output_dir: Path) -> dict:
    """Download Microsoft AdventureWorks BOM data."""
    print("\n=== Downloading Microsoft AdventureWorks ===")

    raw_dir = output_dir / "adventureworks" / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    results = {"downloaded": [], "failed": []}

    for filename, url in ADVENTUREWORKS_FILES.items():
        dest = raw_dir / filename
        if download_file(url, dest):
            results["downloaded"].append(filename)
        else:
            results["failed"].append(filename)

    # Save download metadata
    metadata = {
        "source": "adventureworks",
        "repository": "https://github.com/Microsoft/sql-server-samples",
        "license": "MIT",
        "download_date": datetime.now().isoformat(),
        "files": results["downloaded"],
        "failed": results["failed"],
    }

    metadata_path = raw_dir / "download_metadata.json"
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)

    return results


def download_pdxpert(output_dir: Path) -> dict:
    """Download PDXpert PLM sample package."""
    print("\n=== Downloading PDXpert PLM Sample ===")

    raw_dir = output_dir / "pdxpert" / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    results = {"downloaded": [], "failed": []}

    try:
        print(f"  Downloading: {PDXPERT_ZIP_URL}")
        with urllib.request.urlopen(PDXPERT_ZIP_URL, timeout=60) as response:
            zip_data = response.read()
            print(f"  Downloaded ZIP: {len(zip_data):,} bytes")

            # Extract ZIP contents
            with zipfile.ZipFile(io.BytesIO(zip_data)) as zf:
                for name in zf.namelist():
                    # Only extract CSV files
                    if name.endswith('.csv'):
                        # Flatten directory structure
                        dest_name = Path(name).name
                        dest_path = raw_dir / dest_name
                        with zf.open(name) as src:
                            dest_path.write_bytes(src.read())
                            print(f"  -> Extracted: {dest_name}")
                            results["downloaded"].append(dest_name)

    except Exception as e:
        print(f"  ERROR: {e}")
        results["failed"].append("Example-PDXpertImport.zip")

    # Save download metadata
    metadata = {
        "source": "pdxpert",
        "url": PDXPERT_ZIP_URL,
        "license": "Free evaluation",
        "download_date": datetime.now().isoformat(),
        "files": results["downloaded"],
        "failed": results["failed"],
    }

    metadata_path = raw_dir / "download_metadata.json"
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)

    return results


def download_neo4j(output_dir: Path) -> dict:
    """Download Neo4j BOM sample data from GitHub Gist."""
    print("\n=== Downloading Neo4j BOM Gist ===")

    raw_dir = output_dir / "neo4j" / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    results = {"downloaded": [], "failed": []}

    for filename, url in NEO4J_FILES.items():
        dest = raw_dir / filename
        if download_file(url, dest):
            results["downloaded"].append(filename)
        else:
            results["failed"].append(filename)

    # Save download metadata
    metadata = {
        "source": "neo4j_gist",
        "gist_url": "https://gist.github.com/maxdemarzi/e77145f0a77b7b5f6c9287bc0a96928f",
        "license": "Open source",
        "download_date": datetime.now().isoformat(),
        "files": results["downloaded"],
        "failed": results["failed"],
    }

    metadata_path = raw_dir / "download_metadata.json"
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)

    return results


def main():
    parser = argparse.ArgumentParser(
        description="Download BOM demo data from open source and commercial ERPs"
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        default=Path("test/data"),
        help="Output directory for downloaded files"
    )
    parser.add_argument(
        "--opensource-only",
        action="store_true",
        help="Only download open source ERP data (Odoo, ERPNext)"
    )
    parser.add_argument(
        "--commercial-only",
        action="store_true",
        help="Only download commercial ERP data (AdventureWorks, PDXpert, Neo4j)"
    )

    args = parser.parse_args()

    print(f"Output directory: {args.output.absolute()}")

    total_downloaded = 0
    total_failed = 0

    # Open source ERPs
    if not args.commercial_only:
        results = download_odoo(args.output)
        total_downloaded += len(results["downloaded"])
        total_failed += len(results["failed"])

        results = download_erpnext(args.output)
        total_downloaded += len(results["downloaded"])
        total_failed += len(results["failed"])

    # Commercial ERPs
    if not args.opensource_only:
        results = download_adventureworks(args.output)
        total_downloaded += len(results["downloaded"])
        total_failed += len(results["failed"])

        results = download_pdxpert(args.output)
        total_downloaded += len(results["downloaded"])
        total_failed += len(results["failed"])

        results = download_neo4j(args.output)
        total_downloaded += len(results["downloaded"])
        total_failed += len(results["failed"])

    print(f"\n=== Summary ===")
    print(f"Downloaded: {total_downloaded} files")
    print(f"Failed: {total_failed} files")

    if total_failed > 0:
        print("\nSome downloads failed. Check the error messages above.")
        return 1

    print("\nNext steps:")
    if not args.commercial_only:
        print("  1. Run parsers/odoo_parser.py to convert Odoo XML to CSV")
        print("  2. Run parsers/erpnext_parser.py to convert ERPNext JSON to CSV")
    if not args.opensource_only:
        print("  3. Run parsers/adventureworks_parser.py to convert AdventureWorks CSV")
        print("  4. Run parsers/pdxpert_parser.py to convert PDXpert CSV")
        print("  5. Run parsers/neo4j_parser.py to convert Neo4j Cypher")

    return 0


if __name__ == "__main__":
    exit(main())
