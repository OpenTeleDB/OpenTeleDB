# **贡献指南**

本文主要用于指导新手如何快速玩转OpenTeleDB数据库开源社区。

 欢迎来到OpenTeleDB数据库开源社区！感谢您有意愿为这个项目贡献一份力量。无论是修复一个错别字、提交一个Bug、开发一个新功能，还是完善文档，每一种贡献都值得我们珍惜和感谢。

本指南将引导您完成整个贡献流程。请花费几分钟时间阅读它，这会让您的贡献过程更加顺畅。



## **开始之前**

- 搜索现有议题：在提交Bug或新功能建议前，请先搜索 GitHub Issues，确认它是否已被报告或讨论。
- 熟悉架构：对于代码贡献，请先阅读项目的 Readme.md、ARCHITECTURE.md 和 Development.md 文档，了解项目的核心设计与构建方式。
- 设置开发环境：确保您能成功在本地构建和运行项目。请参考 Installation-Guide.md 中的详细说明。

 

## **安装部署**

参考[OpenTeleDB/docs/Installation-Guide.md at docs · OpenTeleDB/OpenTeleDB · GitHub](https://github.com/OpenTeleDB/OpenTeleDB/blob/docs/docs/Installation-Guide.md)

 

## **如何贡献**

### **报告bug或漏洞**

如果您发现了一个Bug或漏洞，请通过 GitHub Issues 提交报告。为了帮助我们快速定位和解决问题，请提供以下信息：

- 问题描述：清晰的描述问题。

- 复现操作：详细描述如何重现这个Bug的具体操作步骤。
- 预期行为：按计划应该产生的结果。
- 实际行为：实际产生的结果，包括完整的错误日志和跟踪报告。
- 环境信息：操作系统、数据库版本和硬件配置等。
- 可能的解决方案：如果您有其它修复思路，欢迎提出。



### **建议新功能**

我们欢迎任何增强OpenTeleDB开源数据库的想法~请在 GitHub Issues 中提交一个“Feature Request”类型的议题。议题内容请提供以下内容：

-  标题：使用清晰的标题概括功能建议。
- 描述：提供详细的描述，包括这个功能解决了什么痛点或问题？
- 期望解决方案：描述您设想的解决方案，及期待这个功能如何工作？
- 替代方案（可选）：您可列举出其他解决方案？
- 背景信息：提供相关背景，是否有其他数据库实现了类似功能？附上相关链接。



### **贡献代码**

这是我们最期待的贡献方式！请遵循以下操作开发工作流。



## **开发工作流**

**操作步骤**

1. Fork 项目仓库

   点击 GitHub 仓库页面的 "Fork" 按钮，将项目复制到您的 GitHub 账户下。

2. 克隆您的 Fork

   将您Fork后的仓库克隆到本地机器。

   git clone https://github.com/your-username/your-db.git

   cd your-db

3. 配置上游远程

   将原始仓库添加为 upstream 远程仓库，以便轻松同步最新更改。

   git remote add upstream https://github.com/original-org/your-db.git

4. 创建特性分支

   请勿直接在 main 或 master 分支上直接进行修改。您可为您的更改创建一个新的分支，分支名应具有描述性。

   \# 切换到主分支并获取最新代码

   git checkout main

   git fetch upstream

   git rebase upstream/main

    

   \# 创建新分支

   git checkout -b feat/awesome-new-feature  # 或 fix/annoying-bug, docs/update-readme

5. 进行更改并编写测试

   进行您的代码更改。

   注意：

   如果您修改了代码逻辑，请同时编写或更新相应的单元测试或集成测试。高测试覆盖率是我们保持项目稳定的基石。

   确保所有现有测试和新测试都能通过。

   遵循项目的代码风格（通常由 .clang-format, .editorconfig 等文件定义）。

6. 提交更改

   我们鼓励使用清晰、规范的提交信息。

   git add .

   git commit -m "feat(storage): implement awesome new feature" 

   提交信息格式建议遵循 [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/)：

-  feat: 新功能
- fix: Bug修复
- docs: 文档更新
- style: 代码格式调整（不影响逻辑）
- refactor: 代码重构
- test: 测试相关
- chore: 构建过程或辅助工具变动

7. 保护分支同步

   在推送前，确保您的特性分支是最新的，以避免合并冲突。

   git fetch upstream

   git rebase upstream/main

8. 推送更改

   将您的特性分支推送到您的Fork仓库。

   git push origin feat/awesome-new-feature

9. 创建Pull Request

​      (1)  访问您Fork后的GitHub仓库。

​      (2)  选择您刚刚推送的特性分支。

​      (3)  点击 "Compare & pull request"。

​      (4)  填写PR模板：

​      (5)  标题：清晰描述PR的目的。

​     (6)  描述：详细说明更改内容、动机以及如何测试。如果解决了某个Issue，请使用关键字（如 Fixes #123）。

​     (7)  提交Pull Request。

## **代码审查与合并**

核心维护者将会审查您的PR，可能会提出修改意见。

请及时关注评论并参与讨论。根据反馈修改代码后，只需再次提交并推送到同一分支，PR会自动更新。 

所有PR必须通过CI/CD流水线的所有检查（编译、测试、代码风格检查等）。

需要至少两位核心维护者的批准才能合并PR。

合并通常采用 Squash and Merge 方式，将您的所有提交合并为一个整洁的提交记录到主分支。



## **贡献者许可协议**

为了保护项目和用户，我们要求所有贡献者签署贡献者许可协议 (CLA)。

在您的第一个PR被合并之前，我们的CLA助手机器人会引导您完成在线签署流程。

这是一次性操作，签署后您未来的所有贡献都将被覆盖。



## **寻求帮助**

如果您在任何步骤遇到困难，欢迎寻求帮助：

- 在您的PR中留言。
- 加入我们的OpenTeleDB数据库开源社区。
- 欢迎您通过邮件（teledb@chinatelecom.cn）联系我们。

再次感谢您的宝贵时间和贡献！