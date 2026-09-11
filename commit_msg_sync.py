#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
commit_msg_sync.py — 把手动提交的提交信息自动写入「提交信息.md」

用法（提交完成后执行即可）：
    python commit_msg_sync.py                # 把「上次同步之后」到 HEAD 的所有提交写入 md
    python commit_msg_sync.py --count 1      # 只写入最近 1 条提交
    python commit_msg_sync.py --from <sha>   # 指定起始提交（不含该提交）
    python commit_msg_sync.py --dry-run      # 只显示将要写入的内容，不改文件
    python commit_msg_sync.py --md <路径>     # 指定 md 文件（默认：仓库根目录 提交信息.md）

工作原理：
    · md 的「## 二、提交记录」标题下有一行锚点：<!-- commit-sync: <sha> -->
    · 脚本读取锚点 → 取 `git log <锚点>..HEAD` → 把每条提交（时间/短SHA/标题/完整正文/变更文件）
      以「最新在最上面」的顺序插入到该标题下方 → 更新锚点为 HEAD
    · 漏跑多次也没关系：下次运行会把中间所有提交一起补齐；已同步则提示"无新提交"

设计注意：
    · 不向子进程开管道（stdout/stderr 都写临时文件），避免受限环境下命名管道被拒
    · 全部按 UTF-8 读写，md 用 LF 换行；提交正文若含 ``` 会自动加长围栏
    · 只改 md 与锚点，不改动 git 历史
"""

import argparse
import io
import os
import re
import subprocess
import sys
import tempfile

ANCHOR_RE = re.compile(r"<!--\s*commit-sync:\s*([0-9a-fA-F]{7,40})\s*-->")
SECTION_HEAD = "## 二、提交记录"
SEP = "\x1f"          # 字段分隔
REC = "\x1e"          # 记录分隔


# ────────────────────────────── 基础工具 ──────────────────────────────
def find_repo_root(start):
    """向上查找含 .git 的目录"""
    cur = os.path.abspath(start)
    while True:
        if os.path.isdir(os.path.join(cur, ".git")):
            return cur
        parent = os.path.dirname(cur)
        if parent == cur:
            return None
        cur = parent


def git(args, cwd):
    """执行 git；stdout/stderr 分别写临时文件（不开管道），返回 (returncode, stdout, stderr)"""
    out_f = tempfile.NamedTemporaryFile(delete=False, dir=cwd, prefix=".cms_out_", suffix=".tmp")
    err_f = tempfile.NamedTemporaryFile(delete=False, dir=cwd, prefix=".cms_err_", suffix=".tmp")
    out_f.close()
    err_f.close()
    try:
        with io.open(out_f.name, "wb") as fo, io.open(err_f.name, "wb") as fe:
            proc = subprocess.run(["git", "-c", "core.quotepath=false", "--no-pager"] + args,
                                  cwd=cwd, stdout=fo, stderr=fe)
        out = io.open(out_f.name, "rb").read().decode("utf-8", "replace")
        err = io.open(err_f.name, "rb").read().decode("utf-8", "replace")
        return proc.returncode, out, err
    finally:
        for p in (out_f.name, err_f.name):
            try:
                os.remove(p)
            except OSError:
                pass


def has_commit(cwd, sha):
    code, _, _ = git(["cat-file", "-e", sha + "^{commit}"], cwd)
    return code == 0


def is_ancestor(cwd, sha, head):
    code, _, _ = git(["merge-base", "--is-ancestor", sha, head], cwd)
    return code == 0


def head_sha(cwd):
    code, out, _ = git(["rev-parse", "HEAD"], cwd)
    return out.strip() if code == 0 else ""


def resolve_sha(cwd, rev):
    """把任意 rev（短 sha/分支名）解析为完整 sha；失败返回空"""
    code, out, _ = git(["rev-parse", "--verify", "--quiet", rev + "^{commit}"], cwd)
    return out.strip() if code == 0 and out.strip() else ""


# ────────────────────────────── 提交信息读取 ──────────────────────────────
def read_commits(cwd, rev_args):
    """读取提交列表（最新在前）：短SHA/完整SHA/时间/标题/正文。rev_args 为 git log 的位置参数列表"""
    fmt = SEP.join(["%H", "%h", "%ad", "%s", "%B"]) + REC
    code, out, err = git(["log", "--date=format:%Y-%m-%d %H:%M", "--pretty=format:" + fmt] + rev_args, cwd)
    if code != 0:
        raise SystemExit("git log 失败：%s（参数 %s）" % (err.strip(), " ".join(rev_args)))

    commits = []
    for rec in out.split(REC):
        rec = rec.strip("\n")
        if not rec.strip():
            continue
        parts = rec.split(SEP)
        if len(parts) < 5:
            continue
        sha, short, date, subject, body = parts[0], parts[1], parts[2], parts[3], SEP.join(parts[4:])
        commits.append({"sha": sha.strip(), "short": short.strip(), "date": date.strip(),
                        "subject": subject.strip(), "body": body.strip("\n")})
    return commits


def read_changed_files(cwd, sha, limit=40):
    code, out, _ = git(["show", "--no-color", "--name-only", "--pretty=format:", sha], cwd)
    if code != 0:
        return [], 0
    files = [ln.strip() for ln in out.splitlines() if ln.strip()]
    return files[:limit], len(files)


def fence_for(text):
    """按正文里最长反引号串决定围栏长度"""
    longest = 0
    for m in re.finditer(r"`+", text):
        longest = max(longest, len(m.group(0)))
    return "`" * max(3, longest + 1)


def build_entry(cwd, c):
    files, total = read_changed_files(cwd, c["sha"])
    body = c["body"] if c["body"] else c["subject"]
    fence = fence_for(body)

    lines = []
    lines.append("### %s ｜ %s ｜ %s" % (c["date"], c["short"], c["subject"]))
    lines.append("")
    lines.append("%stext" % fence)
    lines.append(body.rstrip())
    lines.append(fence)
    lines.append("")
    if total:
        shown = "、".join(files)
        more = "" if total <= len(files) else "（其余 %d 个见 `git show --stat %s`）" % (total - len(files), c["short"])
        lines.append("- 变更文件（%d）：%s%s" % (total, shown, more))
    else:
        lines.append("- 变更文件：无（如合并提交）")
    lines.append("")
    lines.append("")
    return "\n".join(lines)


# ────────────────────────────── md 读写 ──────────────────────────────
def read_md(path):
    if not os.path.exists(path):
        raise SystemExit("找不到 md 文件：%s" % path)
    return io.open(path, encoding="utf-8").read()


def write_md(path, text):
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def get_anchor(md_text):
    m = ANCHOR_RE.search(md_text)
    return m.group(1) if m else None


def insert_entries(md_text, block):
    """把 block 插到「## 二、提交记录」标题（及其锚点行）之后，保持最新在最上面"""
    lines = md_text.split("\n")
    idx = None
    for i, ln in enumerate(lines):
        if ln.strip().startswith(SECTION_HEAD):
            idx = i
            break
    if idx is None:                      # 没有该章节 → 追加到文末并补标题
        return md_text.rstrip() + "\n\n" + SECTION_HEAD + "\n\n" + block
    j = idx + 1
    # 跳过标题下方的空行、HTML 注释（含锚点）与说明用引用行，使自动条目紧贴"区头"、位于手写条目之前
    while j < len(lines):
        s = lines[j].strip()
        if s == "" or s.startswith("<!--") or s.startswith(">"):
            j += 1
            continue
        break
    head = lines[:j]
    tail = lines[j:]
    while head and head[-1].strip() == "":
        head.pop()
    return "\n".join(head + ["", block.rstrip(), ""] + tail)


def set_anchor(md_text, sha):
    """写入/更新锚点行（放在「## 二、提交记录」标题下第一行）"""
    if ANCHOR_RE.search(md_text):
        return ANCHOR_RE.sub("<!-- commit-sync: %s -->" % sha, md_text, count=1)
    lines = md_text.split("\n")
    for i, ln in enumerate(lines):
        if ln.strip().startswith(SECTION_HEAD):
            lines.insert(i + 1, "")
            lines.insert(i + 2, "<!-- commit-sync: %s -->" % sha)
            lines.insert(i + 3, "")
            return "\n".join(lines)
    return md_text.rstrip() + "\n\n<!-- commit-sync: %s -->\n" % sha


# ────────────────────────────── 主流程 ──────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="把手动提交的提交信息写入 提交信息.md")
    ap.add_argument("--md", default=None, help="md 文件路径（默认：仓库根目录 提交信息.md）")
    ap.add_argument("--count", type=int, default=0, help="只同步最近 N 条提交（默认 0=按锚点增量）")
    ap.add_argument("--from", dest="from_sha", default=None, help="起始提交（不含），默认取 md 锚点")
    ap.add_argument("--dry-run", action="store_true", help="只打印将要写入的内容，不修改文件")
    args = ap.parse_args()

    root = find_repo_root(os.path.dirname(os.path.abspath(__file__))) or find_repo_root(os.getcwd())
    if not root:
        raise SystemExit("未找到 git 仓库（向上查找 .git 失败）")

    md_path = args.md or os.path.join(root, "提交信息.md")
    head = head_sha(root)
    if not head:
        raise SystemExit("git 仓库没有提交（HEAD 为空）")

    md_text = read_md(md_path)
    anchor_raw = args.from_sha or get_anchor(md_text)
    anchor = resolve_sha(root, anchor_raw) if anchor_raw else ""      # 解析为完整 sha（短 sha 也能正确比较）

    # ── 计算提交范围 ──
    if args.count > 0:
        rev_args = ["-n", str(args.count), "HEAD"]
        range_desc = "最近 %d 条（--count）" % args.count
    elif anchor and is_ancestor(root, anchor, "HEAD"):
        if anchor == head:
            print("无新提交：md 锚点已是 HEAD（%s）" % head[:7])
            print("（如需强制重写某条：--count 1 或 --from <sha>）")
            return 0
        rev_args = ["%s..HEAD" % anchor]
        range_desc = "%s..HEAD" % anchor[:7]
    else:
        rev_args = ["-n", "1", "HEAD"]
        range_desc = "最近 1 条（无有效锚点，首次运行）"
        if anchor_raw:
            print("提示：md 锚点 %s 无效或不是 HEAD 的祖先（可能变基/重写），本次仅同步最近 1 条" % anchor_raw[:7])

    commits = read_commits(root, rev_args)
    if not commits:
        print("无新提交（范围 %s 为空）" % range_desc)
        return 0

    blocks = [build_entry(root, c) for c in commits]     # 最新在前
    block = "".join(blocks)

    print("将写入 %d 条提交（范围 %s，最新在最上面）：" % (len(commits), range_desc))
    for c in commits:
        print("  %s  %s  %s" % (c["short"], c["date"], c["subject"]))

    if args.dry_run:
        print("\n---- dry-run：以下内容将插入「%s」下方 ----\n" % SECTION_HEAD)
        print(block)
        return 0

    new_text = insert_entries(md_text, block)
    new_text = set_anchor(new_text, head)
    write_md(md_path, new_text)
    print("\n已写入：%s" % md_path)
    print("锚点已更新：%s -> %s" % ((anchor or "(无)")[:7], head[:7]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
