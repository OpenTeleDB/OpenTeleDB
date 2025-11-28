# OpenTeleDB开发者文档

## 项目介绍

天翼云OpenTeleDB是在中国电信云改数转大背景诞生的，结合TeleDB在电信集团业务和天翼云客户实战打磨的企业级特性，着重解决PostgreSQL存在的并发连接瓶颈，存储空间膨胀，高可用切换的外部依赖等问题。OpenTeleDB本次新增**XProxy**、**XStore**、**XRaft**三大能力，并完美兼容PostgreSQL，基于PostgreSQL的相关业务系统可以无缝的迁移到OpenTeleDB。

- **XProxy**：能够提供十万级原生连接上限的接入能力，支持自动读写分离与负载均衡，实现系统的横向扩展，提升整体性能与可用性，让资源利用更充分。
- **Xstore**：相较于原生PostgreSQL在高并发更新下，因MVCC机制易导致存储空间持续膨胀，且周期性Vacuum回收会引发性能剧烈波动，执行TPCC模型时性能波动常超40%。OpenTeleDB通过XStore存储引擎对存储层进行全链路重构：通过**原位更新**与**Undo日志管理**，从根本杜绝空间膨胀，表空间占用几乎零增长；同时对索引采用原位更新，彻底告别扫表式Vacuum，将执行TPCC模型时性能波动压制在5%以内。
- **XRaft**：在数据库核内完成自动高可用，杜绝脑裂，而不依赖第三方外部组件，结构简单，有效保障业务高可用。

## 文档目录

我们非常欢迎您贡献文档！请您在参与之前，阅读贡献指南，严格遵守文档写作规范，并按流程提交审核。同时，您对文档有任何意见或建议，欢迎通过Issuse提交。

[关于开源数据库OpenTeleDB](/tut/OpenTeleDB-Overview.md)

[安装指南](/tut/Installation-Guide.md)

[快速入门](/tut/quick-start.md)

[简易教程](/tut/Brief-Tutorial.md)

[应用开发指南](/tut/Application-Development-Guide.md)

[版本更新说明](/tut/版本更新说明.md)

[贡献](/tut/Contribute.md)

## 加入我们

作为全球首个运营商级开源数据库，OpenTeleDB的开源，标志着中国数据库开源生态迈入全新阶段。天翼云秉承国云使命担当，将万千次业务实践的经验融入代码，通过OpenTeleDB开放出来与业界共享，帮助企业在数据库的性能、稳定性及开源生态获得更多选择。

如果您也对OpenTeleDB感兴趣~欢迎加入我们！

再次感谢您的宝贵时间！

