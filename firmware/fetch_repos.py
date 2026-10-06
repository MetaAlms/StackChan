import os
import subprocess
import json


def clone_or_update_repo(
    repo_url, path, ref=None, with_submodules=False, patch_paths=None
):
    """Fetch a dependency and apply any patches needed to build StackChan.

    `patch_paths` may hold several patches applied in order. The Aliyun port
    needs a second patch on top of the vendor one, so a single `patch` field is
    no longer enough.
    """
    import os

    if not os.path.exists(path):
        subprocess.run(["git", "clone", repo_url, path], check=True)
    else:
        subprocess.run(["git", "-C", path, "fetch"], check=True)

    if ref:
        subprocess.run(["git", "-C", path, "checkout", ref], check=True)

    if with_submodules:
        subprocess.run(
            ["git", "-C", path, "submodule", "update", "--init", "--recursive"],
            check=True,
        )

    # 应用 patch（按顺序，可能不止一个）
    for patch_path in patch_paths or []:
        patch_full_path = (
            patch_path
            if os.path.isabs(patch_path)
            else os.path.join(os.getcwd(), patch_path)
        )
        # 使用 git apply --check 先检测补丁是否能应用，避免报错。
        # 注意：已经打过补丁时 --check 会失败，这是正常的（重复执行 fetcher）。
        check_result = subprocess.run(
            ["git", "-C", path, "apply", "--check", patch_full_path]
        )
        if check_result.returncode == 0:
            subprocess.run(["git", "-C", path, "apply", patch_full_path], check=True)
            print(f"Applied patch {patch_path} to {path}")
        else:
            print(
                f"Patch {patch_path} cannot be applied cleanly to {path}, "
                "skipped (already applied?)."
            )


def fetch_dependencies():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(script_dir, "repos.json")

    with open(config_path) as f:
        repos = json.load(f)

    for repo in repos:
        repo_path = os.path.join(script_dir, repo["path"])
        branch = repo.get("branch")
        with_submodules = repo.get("with_submodules", False)
        # Accept both "patch": "x.patch" and "patch": ["x.patch", "y.patch"].
        patches = repo.get("patch") or []
        if isinstance(patches, str):
            patches = [patches]
        patches = [
            item if os.path.isabs(item) else os.path.join(script_dir, item)
            for item in patches
        ]
        clone_or_update_repo(repo["url"], repo_path, branch, with_submodules, patches)


if __name__ == "__main__":
    fetch_dependencies()