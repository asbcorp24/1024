Import("env")

from pathlib import Path


def check_assets(source, target, env):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    required = [
        project_dir / "data" / "index.html",
        project_dir / "data" / "help.html",
        project_dir / "data" / "app.js",
        project_dir / "data" / "style.css",
    ]
    missing = [str(path.relative_to(project_dir)) for path in required if not path.exists()]
    if missing:
        print("Missing LittleFS assets: " + ", ".join(missing))


env.AddPreAction("buildprog", check_assets)
