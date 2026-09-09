# GitLab 协作流程（保护主干 + 个人远端分支）

本文面向第一次按该约定开发的同事：说明分支怎么分、本地怎么配、每天敲哪些 git 命令、如何在 GitLab 上提合并请求。

示例名称（**不要**当成仓库里的真实分支名）：

| 角色 | 示例名 | 含义 |
|------|--------|------|
| Git 远端 | `origin` | `git remote -v` 里指向 GitLab 的那个名字；你仓库里可能叫别的 |
| 保护主干 | `main` | 唯一可发布线。禁止普通成员直接 push |
| 个人远端分支 | `work/linxiao` | 每人一条，可自由 push；合并进主干只能走 MR |
| 本地唯一分支 | `main` | 本地只留这一条；**名字虽叫 `main`，跟踪的是个人远端** |

下文命令里的 `origin` / `main` / `work/linxiao` 请换成你们仓库的实际名字。

---

## 1. 约定

```
GitLab main（保护：不能直接 push）
        ▲
        │ 管理员审核并 Merge Request
        │
每人：fetch 主干 → 合进本地 → 开发 → push 到自己的 work/xxx → 开 MR
```

- **主干只进、不出手改**：日常不在 GitLab 网页上直接改 `main`，也不 `git push origin main`。
- **本地只维护一条分支**：避免同时检出主干和个人分支，减少切错线。
- **本地这条线跟踪个人远端**：`git push` / `git pull` 默认都打到 `work/linxiao`，不会误推主干。
- **同步主干必须写全名**：更新发布线时用 `origin/main`，不要指望裸 `git pull` 去拉主干。

本地 `main` 一旦有自己的提交，就会和远端 `main` **暂时不一样**。  
「和主干一致」只发生在：**开工前已合入 `origin/main`，且自己还没开始改**。

---

## 2. 管理员：保护主干（做一次）

GitLab：`Settings → Repository → Protected branches`，保护 `main`：

- Allowed to merge：Maintainers（或指定审核人）
- Allowed to push：**No one**（至少禁止 Developer）
- 禁止 force push、禁止删除该分支

每人在 GitLab 上需要有一条个人分支（如 `work/linxiao`），Developer 对该分支有 push 权限。

---

## 3. 开发者：本地只留一条分支（做一次）

假设当前能 `git fetch`，工作区干净。

```bash
git fetch origin
git switch main
git merge --ff-only origin/main
```

若本地还有多余分支（例如以前的功能线），确认不需要后删除：

```bash
git branch -d 旧分支名
```

未合并时 `-d` 会拒绝；确认不要那些提交再用 `git branch -D 旧分支名`。

把当前本地 `main` 推到个人远端，并设为默认上下游：

```bash
git push -u origin main:work/linxiao
```

检查：

```bash
git remote -v
git branch -vv
```

期望类似：

```text
* main  a1b2c3d [origin/work/linxiao] ...
```

本地只有 `main`；方括号里必须是 **个人远端**，不能是 `origin/main`。

若本地 `main` 仍跟踪 `origin/main`，改过来：

```bash
git branch -u origin/work/linxiao main
```

---

## 4. 日常开发（每次改代码）

### 4.1 开工前：把远端主干合进本地

```bash
git fetch origin
git merge origin/main
```

有冲突：按文件解完后

```bash
git add <解完的文件>
git commit          # 完成这次 merge
```

可选：先看主干比你多了什么：

```bash
git log --oneline HEAD..origin/main
```

### 4.2 修改、提交

```bash
git status
git add <文件>
git commit -m "用一句话说清为什么改，而不是改了哪些文件"
```

提交信息写给审核人看，例如「修复日志采集在缺字段时误报 invalid_state」，不要写「update」。

### 4.3 推到个人远端（不要推主干）

```bash
git push
```

等价于 `git push origin HEAD:work/linxiao`。  
**不要**执行 `git push origin main`。

### 4.4 在 GitLab 创建合并请求

1. GitLab → Merge Requests → New merge request
2. **Source branch**：`work/linxiao`
3. **Target branch**：`main`
4. 填写说明、指派审核人、创建

无法自动合并时：回到 4.1 再合一次 `origin/main`，解决冲突后 `git push`，MR 会自动更新。

### 4.5 管理员审核并合并

审核人在 GitLab 上看 diff、评论、通过后点 Merge。  
合并后 **`origin/main` 才包含这次改动**；个人分支上的提交不会自动消失，也无需每次删除。

### 4.6 合并之后（开发者）

下次开工重复 4.1，即可拿到自己已合进主干的内容，以及其他人已合并的内容。

若希望个人远端和主干重新快进对齐（可选）：

```bash
git fetch origin
git merge origin/main
git push
```

---

## 5. 命令速查

| 目的 | 命令 |
|------|------|
| 更新远端引用 | `git fetch origin` |
| 把主干合进本地 | `git merge origin/main` |
| 只快进、禁止分叉（仅当你确定本地没有独有提交） | `git merge --ff-only origin/main` |
| 看主干多出来的提交 | `git log --oneline HEAD..origin/main` |
| 看自己相对个人远端超前/落后 | `git status -sb` |
| 提交 | `git add …` 然后 `git commit -m "…"` |
| 推个人远端 | `git push` |
| 首次推并设跟踪 | `git push -u origin main:work/linxiao` |
| 改跟踪目标 | `git branch -u origin/work/linxiao` |
| 看本地分支与跟踪 | `git branch -vv` |
| 看工作区 | `git status` |

---

## 6. 禁止与易错

| 不要 | 原因 |
|------|------|
| `git push origin main` | 打保护主干；应被服务器拒绝 |
| 只靠 `git pull` 同步主干 | 本地跟踪的是 `work/linxiao`，`pull` 拉的是自己的远端 |
| 在本地 `main` 上以为自己就是主干 | 本地会超前 `origin/main`，那是待审改动 |
| `git pull origin main` 却以为切到了主干 | 这是把 `origin/main` **合进当前分支**，人还在本地 `main` 上 |
| 对已推送的个人分支 `rebase` 后再强推 | 只有你一个人用 `work/linxiao` 时才可考虑 `push --force-with-lease`；默认用 merge |
| 两人共用同一条 `work/linxiao` | 互相覆盖；一人一条个人远端 |

---

## 7. 冲突怎么处理

开工前或 MR 报冲突时：

```bash
git fetch origin
git merge origin/main
```

Git 会标出冲突文件。打开文件，去掉 `<<<<<<<` / `=======` / `>>>>>>>`，留下正确内容：

```bash
git add <已解决的文件>
git commit
git push
```

不要在冲突未解决时 `git push`。

---

## 8. 新同事从零克隆

```bash
git clone <GitLab 仓库 URL>
cd <仓库目录>
git fetch origin
git switch main
git merge --ff-only origin/main
git push -u origin main:work/自己的名字
```

之后按第 4 节日常开发。若 GitLab 上个人分支已由管理员建好且有提交，也可：

```bash
git clone <GitLab 仓库 URL>
git fetch origin
git switch -c main --track origin/work/linxiao
```

再执行 `git merge origin/main` 对齐主干。

---

## 9. 和「短功能分支」的关系（可选）

默认模型是 **一人一条长期个人远端**（`work/linxiao` 反复 MR 进 `main`）。

若一次需求希望独立审核、合完即删，可从最新主干拉短分支（仍不要直接推 `main`）：

```bash
git fetch origin
git merge origin/main
git switch -c work/linxiao-fix-timeout
git push -u origin work/linxiao-fix-timeout
```

MR：`work/linxiao-fix-timeout` → `main`。合并后可删短分支，本地再切回唯一的 `main`。

长期个人线适合持续交付同一条产品工作线；短分支适合互不相关、需要单独审的改动。团队应选一种作为默认，避免混用到分不清「当前跟踪谁」。
