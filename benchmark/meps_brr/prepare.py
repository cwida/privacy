"""Download/normalize the official 2016 MEPS exercise inputs; no invented samples.

Requires pandas. Download/format conversion is reported separately from DuckDB import.
Sources: https://github.com/HHS-AHRQ/MEPS/tree/master/R
         https://meps.ahrq.gov/mepsweb/data_stats/download_data/pufs/h036brr/h36brr24doc.shtml
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
import urllib.request
import zipfile

import pandas as pd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--brr-dta", type=Path, help="Use an already downloaded h36brr24.dta")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    for name in ("persons.csv", "events.csv", "brr.csv", "manifest.json"):
        if (args.directory / name).exists():
            raise RuntimeError(f"Refusing to overwrite {name}; use a new directory")
    started = time.perf_counter()
    sources = {}

    def load(name):
        url = f"https://meps.ahrq.gov/mepsweb/data_files/pufs/{name}ssp.zip"
        path = args.directory / (name + "ssp.zip")
        if not path.exists():
            urllib.request.urlretrieve(url, path)
        sources[name] = {"url": url, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        with zipfile.ZipFile(path) as archive:
            members = [n for n in archive.namelist() if n.lower().endswith((".ssp", ".xpt"))]
            if len(members) != 1:
                raise RuntimeError(f"Expected one SAS transport file in {path}")
            with archive.open(members[0]) as file:
                frame = pd.read_sas(file, format="xport", encoding="utf-8")
        # pandas XPORT bug #30051: an all-zero IBM float decodes as 2**-260.
        # Match only that exact artifact; do not round legitimate small values.
        frame = frame.replace(2.0 ** -260, 0.0)
        frame["DUPERSID"] = frame.DUPERSID.astype(str).str.strip()
        return frame

    persons = load("h192")
    persons = persons[["DUPERSID", "PANEL", "PERWT16F", "TOTEXP16", "AGELAST"]].rename(
        columns=dict(DUPERSID="person_id", PANEL="panel", PERWT16F="weight",
                     TOTEXP16="expenditure", AGELAST="age"))
    persons.panel = persons.panel.astype(int)
    if persons.person_id.duplicated().any():
        raise RuntimeError("Duplicate person identifiers")
    persons.to_csv(args.directory / "persons.csv", index=False)

    events = []
    for kind, name in (("OB", "h188g"), ("OP", "h188f")):
        frame = load(name)
        frame = frame.loc[frame[kind + "XP16X"] >= 0].copy()
        def payment(suffix):
            if kind == "OB":
                return frame["OB" + suffix + "16X"]
            return frame["OPF" + suffix + "16X"] + frame["OPD" + suffix + "16X"]
        events.append(pd.DataFrame({
            "person_id": frame.DUPERSID, "kind": kind, "physician": frame.SEEDOC,
            "self": payment("SF"), "private": payment("PV") + payment("TR"),
            "medicare": payment("MR"), "medicaid": payment("MD"),
            "other": sum(payment(s) for s in ("OF", "SL", "VA", "OT", "OR", "OU", "WC")),
        }))
    events = pd.concat(events, ignore_index=True)
    if not events.person_id.isin(persons.person_id).all():
        raise RuntimeError("Unmatched event person")
    events.to_csv(args.directory / "events.csv", index=False)

    brr_path = args.brr_dta
    if brr_path is None:
        url = "https://meps.ahrq.gov/mepsweb/data_files/pufs/h36brr24dta.zip"
        archive_path = args.directory / "h36brr24dta.zip"
        if not archive_path.exists():
            urllib.request.urlretrieve(url, archive_path)
        with zipfile.ZipFile(archive_path) as archive:
            member = next(n for n in archive.namelist() if n.lower().endswith(".dta"))
            with archive.open(member) as file:
                brr = pd.read_stata(file, convert_categoricals=False)
        sources["brr"] = {"url": url, "sha256": hashlib.sha256(archive_path.read_bytes()).hexdigest()}
    else:
        brr = pd.read_stata(brr_path, convert_categoricals=False)
        sources["brr"] = {"path": str(brr_path), "sha256": hashlib.sha256(brr_path.read_bytes()).hexdigest()}
    brr = brr.rename(columns={"DUPERSID": "person_id", "PANEL": "panel"})
    brr.person_id = brr.person_id.astype(str).str.strip()
    brr.panel = brr.panel.astype(int)
    cols = [f"BRR{i}" for i in range(1, 129)]
    brr = persons[["person_id", "panel"]].merge(brr[["person_id", "panel"] + cols],
        on=["person_id", "panel"], how="left", validate="one_to_one")
    if not brr[cols].isin([0, 1]).all().all():
        raise RuntimeError("Missing/non-binary BRR flags")
    brr[cols] = brr[cols].astype("uint8")
    brr.to_csv(args.directory / "brr.csv", index=False)
    manifest = dict(year=2016, persons=len(persons), events=len(events), sources=sources,
                    download_conversion_seconds=time.perf_counter() - started)
    (args.directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
