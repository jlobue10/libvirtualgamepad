#!/usr/bin/env python3
"""The capture decoders must tolerate what a real capture can contain: a SET_IDLE / SET_PROTOCOL
control row with an empty payload (classified as SET_REPORT before, IndexError), rows shorter
than the field list, and a stick export with zero or one 0x42 reports (ValueError /
ZeroDivisionError before). Runs both scripts on temporary TSVs; nothing else is touched.
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
failures = 0


def check(ok, text):
    global failures
    print(("PASS " if ok else "FAIL ") + text)
    failures += not ok


def run(script, tsv):
    return subprocess.run([sys.executable, "-I", str(HERE / script), str(tsv)], capture_output=True, text=True, encoding="utf-8")


with tempfile.TemporaryDirectory(prefix="sc26-decoders-") as temp:
    work = pathlib.Path(temp)
    header = "frame\ttime\tdir\tbmRequestType\tbRequest\twValue\twIndex\twLength\tsetup_report_type\tsetup_report_id\tdata_len\tpayload\n"
    set_report = "01" + "87" + "03" + "aabbcc" + "00" * 58  # [id][type][len][payload] padded to 64 bytes
    get_reply = "01" + "ae" + "05" + "01" + "48656c6c" + "00" * 56
    handshake = work / "handshake.tsv"
    handshake.write_text(
        header
        + "1\t0.001\t0x00\t0x21\t0x0a\t0x0000\t0x0002\t0\t\t\t0\t\n"           # SET_IDLE: no payload
        + "2\t0.002\t0x00\t0x21\t0x0b\t0x0000\t0x0002\t0\t\t\t0\t\n"           # SET_PROTOCOL: no payload
        + "3\t0.003\t0x00\t0x21\t0x09\t0x0301\t0x0002\t64\t3\t1\t64\t" + set_report + "\n"
        + "4\t0.004\t0x00\t0xa1\t0x01\t0x0301\t0x0002\t64\t3\t1\t0\t\n"
        + "5\t0.005\t0x01\t\t\t\t\t\t\t\t64\t" + get_reply + "\n"
        + "6\t0.006\t0x00\t0x21\n"                                             # a short row
        + "7\t0.007\t0x00\t0x21\t0x09\t0x0301\t0x0002\t64\t3\t1\t64\t" + "01ae05" + "\n",  # 0xae with no body
        encoding="utf-8")
    result = run("Decode-Sc26Handshake.py", handshake)
    check(result.returncode == 0, "the handshake decoder survives SET_IDLE/SET_PROTOCOL rows without payload, short rows and a bodiless 0xae")
    check("type=0x87" in result.stdout, "the real SET_REPORT is still decoded")
    if result.returncode != 0:
        print(result.stderr[-600:])

    stick_header = "frame\ttime\tendpoint\tdir\tpayload\n"
    report = "42" + "01" + "00" * 8 + "0000" * 4 + "00" * 12 + "00000000" + "00" * 20  # 54 bytes, id 0x42
    for count in (0, 1, 2):
        stick = work / f"stick{count}.tsv"
        stick.write_text(stick_header + "".join(f"{i}\t{i * 0.004:.3f}\t0x81\t0x01\t{report}\n" for i in range(count)), encoding="utf-8")
        result = run("analyze_sc26_stick.py", stick)
        check(result.returncode == 0, f"the stick analyzer survives {count} report(s)")
        if result.returncode != 0:
            print(result.stderr[-600:])

    # The writer's real layout has two payload columns: usbhid.data, else usb.capdata (a hub
    # capture without injected descriptors is not dissected as HID, so only the sixth column is
    # set). A malformed hex cell must be skipped, not fatal.
    raw_header = "frame\ttime\tendpoint\tdir\tpayload\tpayload_raw\n"
    stick = work / "stick_capdata.tsv"
    stick.write_text(raw_header + "".join(f"{i}\t{i * 0.004:.3f}\t0x81\t0x01\t\t{report}\n" for i in range(2))
                     + "9\t0.040\t0x81\t0x01\tzz\t\n", encoding="utf-8")
    result = run("analyze_sc26_stick.py", stick)
    check(result.returncode == 0 and "0x42 reports: 2" in result.stdout,
          "the stick analyzer reads usb.capdata payloads and skips a malformed hex cell")
    if result.returncode != 0:
        print(result.stderr[-600:])

print(f"{failures} failures")
sys.exit(1 if failures else 0)
