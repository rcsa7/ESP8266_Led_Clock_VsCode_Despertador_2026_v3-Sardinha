from pathlib import Path
import os
import subprocess
import tempfile

import markdown


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "manual-wifi.md"
OUTPUT = ROOT / "docs" / "manual-wifi.pdf"


def find_browser() -> Path:
    candidates = [
        Path(os.environ.get("PROGRAMFILES(X86)", ""))
        / "Microsoft/Edge/Application/msedge.exe",
        Path(os.environ.get("PROGRAMFILES", ""))
        / "Google/Chrome/Application/chrome.exe",
        Path(os.environ.get("PROGRAMFILES(X86)", ""))
        / "Google/Chrome/Application/chrome.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("Chrome ou Microsoft Edge nao encontrado.")


def build_html() -> str:
    body = markdown.markdown(
        SOURCE.read_text(encoding="utf-8"),
        extensions=["tables"],
    )
    base_url = SOURCE.parent.as_uri() + "/"
    return f"""<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<base href="{base_url}">
<style>
@page {{ size: A4; margin: 16mm 14mm 17mm; }}
* {{ box-sizing: border-box; }}
body {{
  color: #1b2924;
  font-family: "Segoe UI", Arial, sans-serif;
  font-size: 10.5pt;
  line-height: 1.45;
  margin: 0;
}}
h1 {{
  border-bottom: 4px solid #287a5a;
  color: #173d31;
  font-size: 25pt;
  margin: 0 0 18px;
  padding-bottom: 10px;
}}
h2 {{
  border-bottom: 1px solid #b8cbc3;
  color: #176b50;
  font-size: 17pt;
  margin: 24px 0 10px;
  padding-bottom: 5px;
  break-after: avoid;
}}
h3 {{ color: #214e40; font-size: 13pt; margin: 18px 0 7px; break-after: avoid; }}
p {{ margin: 7px 0; }}
ul, ol {{ margin: 7px 0 10px; padding-left: 24px; }}
li {{ margin: 3px 0; }}
table {{ border-collapse: collapse; font-size: 9.5pt; margin: 12px 0; width: 100%; }}
th {{ background: #287a5a; color: white; text-align: left; }}
th, td {{ border: 1px solid #aebfb8; padding: 7px 8px; vertical-align: top; }}
tr:nth-child(even) td {{ background: #f1f7f4; }}
blockquote {{
  background: #fff7df;
  border-left: 5px solid #d6a72e;
  margin: 12px 0;
  padding: 8px 12px;
}}
blockquote p {{ margin: 2px 0; }}
code {{ background: #edf2f0; border-radius: 3px; padding: 1px 4px; }}
pre {{ background: #18241f; color: white; padding: 10px 12px; break-inside: avoid; }}
pre code {{ background: transparent; padding: 0; }}
img {{
  border: 1px solid #c7d4ce;
  display: block;
  height: auto;
  margin: 12px auto 16px;
  max-height: 122mm;
  max-width: 100%;
  object-fit: contain;
  break-inside: avoid;
}}
a {{ color: #176b50; }}
</style>
</head>
<body>{body}</body>
</html>"""


def main() -> None:
    browser = find_browser()
    with tempfile.TemporaryDirectory(prefix="manual-wifi-") as temp_dir:
        html_path = Path(temp_dir) / "manual-wifi.html"
        html_path.write_text(build_html(), encoding="utf-8")
        subprocess.run(
            [
                str(browser),
                "--headless",
                "--disable-gpu",
                "--allow-file-access-from-files",
                "--no-pdf-header-footer",
                f"--print-to-pdf={OUTPUT}",
                html_path.as_uri(),
            ],
            check=True,
        )

    if not OUTPUT.is_file() or OUTPUT.stat().st_size < 10_000:
        raise RuntimeError("O PDF nao foi gerado corretamente.")
    if not OUTPUT.read_bytes().startswith(b"%PDF-"):
        raise RuntimeError("O arquivo gerado nao possui uma assinatura PDF valida.")
    print(f"PDF gerado: {OUTPUT} ({OUTPUT.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()