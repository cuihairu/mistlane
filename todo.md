- [x] 审核整改（2026-10-18 独立审核，见 docs/review/luna-review-2026-10-18.md）：
      按审核报告的三张表逐项处理，**先核对再动手**，每项给 file:line 证据。
      1) 表一「缺陷」：按风险排序修，高风险优先
      2) 表二「文档与实现不一致」：以代码为准改文档，或明确标注为规划中
         **禁止为对齐文档去改业务代码**
      3) 表三「值得补的能力」：只做评估与分期建议，**不实现**（需用户拍板）
      每完成一项勾掉并写证据；全部测试通过后 commit+push；不 bump 版本号。
      整改记录见 docs/review/luna-review-response-2026-10-18.md（表一 7 项：6 修 1 复核；
      表二 4 项文档更新+压测手册实测 16000/16000；表三 7 项评估分期，未实现）。
      证据：src/node.cpp（探针写缓冲 shared_ptr 保活、Peer::finish/retire、
      buffer_{256}+max_peers 64、tick 同步 set_ready、reject 计数、parse_address 五处）；
      src/test_cases.cpp probe.control 扩展；src/scaffold.cpp 文案更正；ctest 14/14 绿；
      MariaDB 集成测试本环境复跑通过；docs:build 通过。
