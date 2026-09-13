#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
commit_msg_sync.py — 把手动提交的提交信息写入「提交信息.md」

用法（提交完成后执行）：
    python commit_msg_sync.py                 # 增量：把「上次同步之后」到 HEAD 的提交写入 md
    python commit_msg_sync.py --all           # 全量：把**整个 git 历史**写入 md（可重复执行，不会重复追加）
    python commit_msg_sync.py --dry-run       # 只打印将要写入的内容，不改文件
    python commit_msg_sync.py --status        # 只看同步状态（锚点/已收录条数/待同步条数）
    python commit_msg_sync.py --count 1       # 只写最近 1 条
    python commit_msg_sync.py --from <sha>    # 指定起始提交（不含）
    python commit_msg_sync.py --no-files      # 不写"变更文件"清单（历史长时更精简）
    python commit_msg_sync.py --max 100       # 最多写入 100 条（配合 --all 控制文件大小）
    python commit_msg_sync.py --md <路径>      # 指定 md 文件（默认仓库根目录 提交信息.md）

工作原理：
    · md 的「## 二、提交记录」章节内有一对标记（脚本管理）：
          <!-- auto-sync-begin -->   … 自动区：各次提交信息 …   <!-- auto-sync-end -->
      以及一行锚点：<!-- commit-sync: <sha> -->（记录已同步到哪）
    · 增量：读锚点 → `git log <锚点>..HEAD` → 新条目并入自动区**顶部** → 锚点更新为 HEAD
    · 全量（--all）：自动区 = 整个 git 历史（HEAD 全部提交，最新在最上面）
    · 自动区按"短 SHA"去重：**重复执行不会产生重复条目**（可放心多跑）
    · 手写内容放在 <!-- auto-sync-end --> 之后，脚本不会改动
    · 漏跑多次也没关系：下次增量运行会把中间所有提交一起补齐

设计注意：
    · 不向子进程开管道（stdout/stderr 都写临时文件），避免受限环境下命名管道被拒
    · 全部按 UTF-8 读写，md 用 LF 换行；提交正文若含 ``` 会自动加长围栏
    · 只改 md（自动区/锚点），不改动 git 历史
"""

import argparse
import io
import os
import re
import subprocess
import sys
import tempfile

ANCHOR_RE = re.compile(r"<!--\s*commit-sync:\s*([0-9a-fA-F]{7,40})\s*-->")
AUTO_BEGIN = "<!-- auto-sync-begin -->"
AUTO_END = "<!-- auto-sync-end -->"
SECTION_HEAD = "## 二、提交记录"
ENTRY_HEAD_RE = re.compile(r"^###\s+(.*?)\s*｜\s*([0-9a-fA-F]{7,40})\s*｜\s*(.*)$")
SEP = "\x1f"          # git log 字段分隔
REC = "\x1e"          # git log 记录分隔


# ────────────────────────────── git 基础 ──────────────────────────────
def find_repo_root(start):
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


def head_sha(cwd):
    code, out, _ = git(["rev-parse", "HEAD"], cwd)
    return out.strip() if code == 0 else ""


def resolve_sha(cwd, rev):
    code, out, _ = git(["rev-parse", "--verify", "--quiet", rev + "^{commit}"], cwd)
    return out.strip() if code == 0 and out.strip() else ""


def is_ancestor(cwd, sha, head):
    code, _, _ = git(["merge-base", "--is-ancestor", sha, head], cwd)
    return code == 0


def commit_count(cwd):
    code, out, _ = git(["rev-list", "--count", "HEAD"], cwd)
    try:
        return int(out.strip())
    except ValueError:
        return 0


# ────────────────────────────── 提交信息读取 ──────────────────────────────
def read_commits(cwd, rev_args):
    """读取提交列表（最新在前）：完整/短SHA、时间、标题、正文"""
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
        commits.append({"sha": parts[0].strip(), "short": parts[1].strip(), "date": parts[2].strip(),
                        "subject": parts[3].strip(), "body": SEP.join(parts[4:]).strip("\n")})
    return commits


def read_changed_files(cwd, sha, limit):
    if limit <= 0:
        return [], 0
    code, out, _ = git(["show", "--no-color", "--name-only", "--pretty=format:", sha], cwd)
    if code != 0:
        return [], 0
    files = [ln.strip() for ln in out.splitlines() if ln.strip()]
    return files[:limit], len(files)


def fence_for(text):
    """按正文里最长的反引号串决定围栏长度，避免正文里的 ``` 破坏 md 结构"""
    longest = 0
    for m in re.finditer(r"`+", text):
        longest = max(longest, len(m.group(0)))
    return "`" * max(3, longest + 1)


def build_entry(cwd, c, file_limit):
    body = c["body"] if c["body"] else c["subject"]
    fence = fence_for(body)
    lines = ["### %s ｜ %s ｜ %s" % (c["date"], c["short"], c["subject"]), "",
             "%stext" % fence, body.rstrip(), fence, ""]
    if file_limit > 0:
        files, total = read_changed_files(cwd, c["sha"], file_limit)
        if total:
            more = "" if total <= len(files) else "（其余 %d 个见 `git show --stat %s`）" % (total - len(files), c["short"])
            lines.append("- 变更文件（%d）：%s%s" % (total, "、".join(files), more))
        else:
            lines.append("- 变更文件：无（如合并提交）")
        lines.append("")
    lines.append("")
    return "\n".join(lines)


# ────────────────────────────── md 结构 ──────────────────────────────
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


def set_anchor(md_text, sha):
    if ANCHOR_RE.search(md_text):
        return ANCHOR_RE.sub("<!-- commit-sync: %s -->" % sha, md_text, count=1)
    lines = md_text.split("\n")
    for i, ln in enumerate(lines):
        if ln.strip().startswith(SECTION_HEAD):
            lines[i + 1:i + 1] = ["", "<!-- commit-sync: %s -->" % sha, ""]
            return "\n".join(lines)
    return md_text.rstrip() + "\n\n<!-- commit-sync: %s -->\n" % sha


def get_auto_inner(md_text):
    b = md_text.find(AUTO_BEGIN)
    e = md_text.find(AUTO_END)
    if b < 0 or e < 0 or e < b:
        return None
    return md_text[b + len(AUTO_BEGIN):e]


def parse_entries(inner):
    """把自动区文本解析为 [(短SHA, 条目文本)]，保持文件顺序（新 → 旧）"""
    items = []
    if not inner:
        return items
    cur_sha, cur = None, []
    for ln in inner.split("\n"):
        m = ENTRY_HEAD_RE.match(ln)
        if m:
            if cur_sha:
                items.append((cur_sha, "\n".join(cur).rstrip() + "\n"))
            cur_sha, cur = m.group(2), [ln]
        elif cur_sha is not None:
            cur.append(ln)
    if cur_sha:
        items.append((cur_sha, "\n".join(cur).rstrip() + "\n"))
    return items


def ensure_auto_block(md_text):
    """确保自动区标记存在。缺失时：在章节说明行之后创建空自动区（已有内容留在其下，脚本不动）"""
    if get_auto_inner(md_text) is not None:
        return md_text
    lines = md_text.split("\n")
    idx = None
    for i, ln in enumerate(lines):
        if ln.strip().startswith(SECTION_HEAD):
            idx = i
            break
    if idx is None:                                   # 连章节都没有 → 追加到文末
        return md_text.rstrip() + "\n\n%s\n\n%s\n%s\n" % (SECTION_HEAD, AUTO_BEGIN, AUTO_END)
    j = idx + 1
    while j < len(lines):                             # 跳过空行/注释（锚点等）/引用说明行
        s = lines[j].strip()
        if s == "" or s.startswith("<!--") or s.startswith(">"):
            j += 1
            continue
        break
    lines[j:j] = [AUTO_BEGIN, AUTO_END, ""]
    return "\n".join(lines)


def render_auto(entries):
    body = "".join(t if t.endswith("\n") else t + "\n" for _, t in entries)
    return "\n" + body if body else "\n"


def replace_auto(md_text, entries):
    lines = md_text.split("\n")
    b = next((i for i, ln in enumerate(lines) if AUTO_BEGIN in ln), None)
    e = next((i for i, ln in enumerate(lines) if AUTO_END in ln), None)
    if b is None or e is None or e < b:
        raise SystemExit("md 中缺少自动区标记（%s / %s）" % (AUTO_BEGIN, AUTO_END))
    inner_lines = render_auto(entries).strip("\n").split("\n") if entries else []
    new_lines = lines[:b + 1] + inner_lines + [""] + lines[e:]
    # 去掉自动区前后多余空行（保证只有 1 个空行分隔）
    while len(new_lines) > b + 2 and new_lines[b + 1].strip() == "" and new_lines[b + 2].strip() == "":
        del new_lines[b + 1]
    while e < len(new_lines) - 1 and new_lines[e - 1].strip() == "" and new_lines[e - 2].strip() == "":
        del new_lines[e - 1]
        e -= 1
    return "\n".join(new_lines)


# ────────────────────────────── 主流程 ──────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="把手动提交的提交信息写入 提交信息.md")
    ap.add_argument("--md", default=None, help="md 文件路径（默认：仓库根目录 提交信息.md）")
    ap.add_argument("--all", action="store_true", help="全量：写入整个 git 历史（可重复执行，不重复追加）")
    ap.add_argument("--count", type=int, default=0, help="只同步最近 N 条提交")
    ap.add_argument("--from", dest="from_sha", default=None, help="起始提交（不含），默认取 md 锚点")
    ap.add_argument("--max", dest="max_entries", type=int, default=0, help="最多写入 N 条（0=不限制）")
    ap.add_argument("--no-files", action="store_true", help="不写变更文件清单")
    ap.add_argument("--file-limit", type=int, default=40, help="每条提交最多列出多少个变更文件（默认 40）")
    ap.add_argument("--dry-run", action="store_true", help="只打印将要写入的内容，不修改文件")
    ap.add_argument("--status", action="store_true", help="只显示同步状态，不修改文件")
    args = ap.parse_args()

    root = find_repo_root(os.path.dirname(os.path.abspath(__file__))) or find_repo_root(os.getcwd())
    if not root:
        raise SystemExit("未找到 git 仓库（向上查找 .git 失败）")
    md_path = args.md or os.path.join(root, "提交信息.md")
    file_limit = 0 if args.no_files else max(0, args.file_limit)

    head = head_sha(root)
    if not head:
        raise SystemExit("git 仓库没有提交（HEAD 为空）")
    total_commits = commit_count(root)

    md_text = read_md(md_path)
    md_text = ensure_auto_block(md_text)
    existing = parse_entries(get_auto_inner(md_text))
    anchor_raw = args.from_sha or get_anchor(md_text)
    anchor = resolve_sha(root, anchor_raw) if anchor_raw else ""

    # ── --status：只看状态 ──
    if args.status:
        pending = 0
        if anchor and is_ancestor(root, anchor, "HEAD") and anchor != head:
            pending = len(read_commits(root, ["%s..HEAD" % anchor]))
        print("仓库：%s" % root)
        print("md  ：%s" % md_path)
        print("HEAD：%s（git 历史共 %d 条）" % (head[:7], total_commits))
        print("锚点：%s" % (anchor[:7] if anchor else "(无)"))
        print("自动区已收录：%d 条" % len(existing))
        print("待同步：%d 条" % pending)
        return 0

    # ── 计算本次写入范围 ──
    if args.all:
        rev_args = ["HEAD"]
        range_desc = "全部历史（%d 条）" % total_commits
    elif args.count > 0:
        rev_args = ["-n", str(args.count), "HEAD"]
        range_desc = "最近 %d 条（--count）" % args.count
    elif anchor and is_ancestor(root, anchor, "HEAD"):
        if anchor == head:
            print("无新提交：md 锚点已是 HEAD（%s）；自动区已收录 %d 条" % (head[:7], len(existing)))
            print("（重建全部历史：--all；强制写某条：--count 1 或 --from <sha>）")
            return 0
        rev_args = ["%s..HEAD" % anchor]
        range_desc = "%s..HEAD" % anchor[:7]
    else:
        rev_args = ["-n", "1", "HEAD"]
        range_desc = "最近 1 条（无有效锚点，首次运行）"
        if anchor_raw:
            print("提示：md 锚点 %s 无效或不是 HEAD 的祖先（可能变基/重写），本次仅同步最近 1 条" % anchor_raw[:7])

    commits = read_commits(root, rev_args)
    if args.max_entries > 0:
        commits = commits[:args.max_entries]
    if not commits:
        print("无新提交（范围 %s 为空）" % range_desc)
        return 0

    new_entries = [(c["short"], build_entry(root, c, file_limit)) for c in commits]
    new_shas = {s.lower() for s, _ in new_entries}
    # 新条目在前（最新在最上面）；自动区里已有的旧条目按原顺序保留，同 SHA 不重复
    merged = new_entries + [(s, t) for s, t in existing if s.lower() not in new_shas]

    print("将写入 %d 条提交（范围 %s，最新在最上面）；写完后自动区合计 %d 条" %
          (len(new_entries), range_desc, len(merged)))
    for c in commits[:10]:
        print("  %s  %s  %s" % (c["short"], c["date"], c["subject"]))
    if len(commits) > 10:
        print("  …（其余 %d 条）" % (len(commits) - 10))

    if args.dry_run:
        print("\n---- dry-run：以下内容将写入自动区（%s … %s 之间）----\n" % (AUTO_BEGIN, AUTO_END))
        print(render_auto(merged).strip("\n"))
        return 0

    new_text = replace_auto(md_text, merged)
    new_text = set_anchor(new_text, head)
    write_md(md_path, new_text)
    print("\n已写入：%s（%.1f KB）" % (md_path, os.path.getsize(md_path) / 1024.0))
    print("锚点已更新：%s -> %s" % ((anchor or "(无)")[:7], head[:7]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
