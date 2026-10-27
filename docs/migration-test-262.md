# 跨版本函数迁移：1.21.51.02 → 26.2 实战记录

目标：把 1.21.51.02 中已确认的非法附魔 hook 点迁移到 `D:\mc-libs\26.2` 的 `arm64-v8a.so`。

## 一、迁移结果汇总

| 函数 | 1.21.51.02 RVA | 26.2 RVA | 迁移方式 |
|---|---|---|---|
| EnchantCommand::execute | `0x5D251D0` | `0x93E6D6C` | 字符串锚点 `commands.enchant.*` |
| 等级门（invalidLevel 前分支） | `0x5D253A4` `TBZ W0,#0` | `0x93E7360` `CBNZ W8, loc_93E73B4` | 字符串锚点定位到执行器后，反汇编找 `ADRL "invalidLevel"` 前的校验分支 |
| 兼容门（cantEnchant 前分支） | `0xB631E4C` 函数入口 | `0x93E845C` `CBZ W9, loc_93E847C` | 同上，找 `ADRL "cantEnchant"` 前的校验分支 |

### 已验证的 26.2 关键字节（文件偏移 = RVA）
```
0x93E7360  A8 02 00 35   CBNZ W8, loc_93E73B4   ← 等级门（W8!=0 → invalidLevel）
0x93E845C  09 01 00 34   CBZ  W9, loc_93E847C   ← 兼容门（W9==0 → 跳过 cantEnchant）
0x93E73CC  E6 E9 88 95   BL   sub_F621B64       ← 错误输出（invalidLevel）
0x93E8478  BB E5 88 95   BL   sub_F621B64       ← 错误输出（cantEnchant）
```

### 版本差异要点
- **EnchantCommand::execute** 从 4648B 增大到 8480B（重构，校验逻辑内联/拆分）
- **ItemStack 结构稳定**：`sub_10B09D40`（isNull 类比）字段布局 `+34/+24/+32/+72/+104` 与旧版 `sub_B8029B8` 完全一致
- 附魔组件类（`addEnchantment`）在新版疑似并入大型类（`0x110B0458` 40404B），未做最终确认

---

## 二、方法论：跨版本快速找回函数 RVA

### 推荐流程（按可靠性从高到低）

#### 1. 字符串锚点法（最可靠，首选）
```
1. 在新版本 DB 的 constants 表搜索旧版本已知的唯一字符串
   例：'commands.enchant.cantEnchant' → func 146816
2. 该函数 = 新版本的目标函数（EnchantCommand::execute）
3. 用 IDA headless 反编译该函数，搜索特征字符串出现处
4. 在特征字符串的 ADRL 指令前找校验分支（CBZ/CBNZ/TBZ）
```
适用：命令执行器、错误处理、注册表等含唯一字符串的函数。

#### 2. 函数签名 + 调用关系法
```
1. 旧版确认：sub_X 调用 sub_Y（Y 含特征字符串）
2. 新版：找含同字符串的函数，反查其调用者
```
例：`sub_F621B64` 输出所有 `commands.enchant.*` 错误 → 其调用者 = 执行器。

#### 3. 字节特征法（仅限"不依赖地址的稳定指令"）
```
- 单条指令字节（如 TBZ W0,#0 = 60 3B 00 36）：误报极高，禁止单独使用
  实测：26.2 中 60 3B 00 36 命中 V8 引擎的 AsmJsParser::ValidateCall（0x7194684）
- 8+ 字节连续指令：误报低，但仍需用调用者上下文甄别
  实测：sub_B631E4C 入口 3F 00 00 71 E8 07 9F 1A 命中 5 处，仅 1 处是真身附近
```

### 已证伪的路径（勿重复）
| 尝试 | 结果 |
|---|---|
| 旧版函数 RVA 直接复用 | ❌ 版本间重排，全部失效 |
| 单条 TBZ 字节搜索 | ❌ V8 误报 |
| 搜索 `0x190`/`0x1A0` 偏移 | ❌ 误报 tree_feature / cohtml / JSON parser |
| `+400`/`+416` 偏移搜索 | ❌ 2593 个 cohtml/renoir 函数误报 |

### 操作要点
1. **先建索引**：`CREATE INDEX ON functions(address)`（26.2 库 612K 函数，无索引查询超时）
2. **DB 覆盖不全**：`sub_5D251D0`/`sub_98EF9F8` 等旧版函数不在 DB（仅 IDA headless 反编译），需用 `.i64` 副本跑 IDA
3. **26.2 的 .i64 可复制后 headless 打开**（4.4GB），原库若被交互式 IDA 锁定则用副本
4. **验证段映射**：确认 `.text` 段 `file offset == vaddr`（26.2 首 LOAD 段 diff=0），patch 地址 = RVA = 文件偏移

---

## 三、26.2 待完成项
- [ ] 确认 26.2 的 `addEnchantment`（`sub_8F73120` 类比物）及脚本路径校验器位置
- [ ] 验证 `0x93E7360` NOP 后等级门是否真正绕过（需真机/模拟器测试）
- [ ] 更新 `minecraft_world_hook.c` 的多版本 patch 表（按版本签名选择 RVA）
