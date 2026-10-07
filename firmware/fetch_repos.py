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
        # 先用 --check 判断能否正向应用。
        check_result = subprocess.run(
            ["git", "-C", path, "apply", "--check", patch_full_path]
        )
        if check_result.returncode == 0:
            subprocess.run(["git", "-C", path, "apply", patch_full_path], check=True)
            print(f"Applied patch {patch_path} to {path}")
            continue

        # 正向失败有两种截然不同的原因，必须区分，否则一个过期的补丁会被
        # 静默跳过，干净重建就会产出缺少该功能的固件：
        #   1. 补丁已经打过（重复执行 fetcher）——反向 --check 会成功，跳过是对的
        #   2. 补丁真的打不上（上下文漂移／与其他补丁冲突）——必须硬失败
        reverse_check = subprocess.run(
            ["git", "-C", path, "apply", "--reverse", "--check", patch_full_path]
        )
        if reverse_check.returncode == 0:
            print(f"Patch {patch_path} already applied to {path}, skipping")
            continue

        raise SystemExit(
            f"ERROR: patch {patch_path} cannot be applied to {path}, and it is "
            f"NOT already applied. The tree would silently miss this change.\n"
            f"Regenerate the patch against the post-vendor state, or fix the "
            f"conflicting hunk.\n"
            f"Diagnose with:\n"
            f"  git -C {path} apply --check {patch_full_path}"
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