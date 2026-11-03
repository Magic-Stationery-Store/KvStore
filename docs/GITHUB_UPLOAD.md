# 把项目上传到 GitHub（手把手）

> 当前状态：本地仓库已初始化，并完成首次提交 `d941f11`（53 个文件）。
> 你只需要在 GitHub 建空仓库，然后执行第 4–5 步的 `git remote add` + `git push`。
> 如果想从头再来一遍，按第 3 步重新初始化即可。

> 前提：项目根目录是 `E:\VsCode\Cmake\kvstore`，Git 已安装（`git --version` 能输出版本）。

## 1. 先确认哪些东西不能提交

本项目里有三类**不应该进仓库**的内容：

| 目录 / 文件 | 原因 |
| --- | --- |
| `.qt/`、`.toolchains/` | 本地 Qt SDK、MSYS2、虚拟环境，几个 GB |
| `build*/` | 编译产物 |
| `*.log`、`*.sst`、`*.imm`、`demo-data/` | 运行产生的数据库文件 |

根目录的 `.gitignore` 已经排除了它们。提交前用 `git status` 检查，**只应看到源码、测试、文档、CMake 文件**。

## 2. 配置 Git 身份（只需一次）

```bash
git config --global user.name "你的名字"
git config --global user.email "你的邮箱"
```

> 提交记录里的邮箱是公开的。建议使用 GitHub 提供的
> `12345678+username@users.noreply.github.com`，避免暴露 QQ/学校邮箱：
> GitHub → Settings → Emails → Keep my email addresses private，页面会给出专用地址。
> 修改后对**之后的提交**生效；已有提交可用 `git commit --amend --reset-author` 修正。

## 3. 初始化仓库并提交

在项目根目录执行：

```bash
cd /e/VsCode/Cmake/kvstore      # Git Bash / WSL 路径；PowerShell 用 E:\VsCode\Cmake\kvstore
git init -b main
git add .
git status                       # 仔细看一遍，确认没有 .qt / .toolchains / build
git commit -m "feat: LSM-tree KV store with WAL, MANIFEST and Qt console"
```

如果 `git status` 里出现了大目录，先确认 `.gitignore` 生效：

```bash
git check-ignore -v .qt .toolchains build-ubuntu
```

## 4. 在 GitHub 上创建空仓库

1. 打开 <https://github.com/new>；
2. Repository name 填 `kvstore`；
3. **不要**勾选 "Add a README file" / "Add .gitignore"（否则会与本地冲突）；
4. 点 Create repository，复制页面上的仓库地址。

## 5. 关联远程仓库并推送

```bash
git remote add origin https://github.com/<你的用户名>/kvstore.git
git push -u origin main
```

### 认证方式（GitHub 不支持密码）

**方式 A：Personal Access Token（最快）**

1. GitHub → Settings → Developer settings → Personal access tokens →
   Tokens (classic) → Generate new token；
2. 勾选 `repo` 权限，生成后复制；
3. `git push` 提示输入密码时，粘贴 token（不是 GitHub 登录密码）。

**方式 B：SSH（推荐，长期免输密码）**

```bash
ssh-keygen -t ed25519 -C "你的邮箱"     # 一路回车
cat ~/.ssh/id_ed25519.pub               # 复制输出
```

把公钥粘贴到 GitHub → Settings → SSH and GPG keys → New SSH key，然后：

```bash
ssh -T git@github.com                   # 看到 "Hi <user>!" 即成功
git remote set-url origin git@github.com:<你的用户名>/kvstore.git
git push -u origin main
```

## 6. 仓库设置（提升简历观感）

- **About**：Description 填
  `LSM-tree KV storage engine in C++17: WAL, immutable MemTable, L0 SSTable, MANIFEST commit, Qt console`；
- **Topics**：`lsm-tree` `key-value-store` `cpp17` `qt` `storage-engine` `cmake`；
- 确认 README 里的截图能正常显示（`docs/screenshot.png` 必须一起提交）；
- 加一个 `LICENSE`（MIT 最省事）：
  GitHub 仓库页 → Add file → Create new file → 文件名 `LICENSE` → 选择 MIT 模板。

## 7. 日常提交流程

```bash
git status
git diff                     # 看改了什么
git add <文件或 .>
git commit -m "feat: add bloom filter to SSTable lookup"
git push
```

提交信息用 `feat:` / `fix:` / `docs:` / `test:` / `refactor:` 前缀，历史会好看很多。

## 8. 常见问题

| 问题 | 解决 |
| --- | --- |
| `remote origin already exists` | `git remote set-url origin <新地址>` |
| `failed to push some refs` / `fetch first` | `git pull --rebase origin main` 后再 push |
| 误提交了 `.qt` 或 `build` | `git rm -r --cached .qt .toolchains build-ubuntu`，提交后再检查 `.gitignore` |
| 文件太大被拒（>100MB） | 说明提交了 SDK/数据库；用 `git rm --cached` 移除，必要时用 `git filter-repo` 清理历史 |
| 中文路径显示乱码 | `git config --global core.quotepath false` |
| 换行符警告 | 项目里加 `.gitattributes`：`* text=auto` |
| GitHub 访问慢 | 用 SSH 的 443 端口：`ssh -T -p 443 git@ssh.github.com`，并配置 `~/.ssh/config` |

## 9. 推上去之后检查

```bash
git log --oneline -5
git remote -v
```

浏览器打开仓库，确认：

- [ ] README 正常渲染，截图可见；
- [ ] 没有 `.qt`、`.toolchains`、`build*`、`*.log`；
- [ ] 目录结构清晰（`src/`、`tests/`、`qt-demo/`、`docs/`）；
- [ ] 提交历史不是一次巨型 commit（后续继续小步提交即可）。

## 10. 简历上怎么写链接

- 仓库：`github.com/<user>/kvstore`
- 如果做了博客/演示视频，放在 README 顶部；
- 面试前把仓库设为 Public，并固定（Pin）到 GitHub 主页。
