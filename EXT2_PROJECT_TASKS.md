# Ext2 File System Project Task Summary

## 基础功能（15 分）

### 1) 挂载与超级块解析（4 pts）
- 已完成内容：实现 Ext2 探测与初始化流程，解析超级块、块组描述符表并缓存；挂载时自动识别 Ext2 并作为根文件系统接入 VFS。
- 主要文件：
  - `os/fs/ext2.c`（超级块/块组描述符读取与缓存、块大小计算）
  - `os/fs/ext2_vfs.c`（根挂载与 VFS 接入）
  - `os/fs/ext2.h`（Ext2 结构体与接口声明）

### 2) 文件创建与读写（5 pts）
- 已完成内容：实现 Ext2 文件读写路径，支持 `read/write/touch`，覆盖写、小文件写入、稀疏文件写入与大小更新。
- 主要文件：
  - `os/fs/ext2.c`（数据块读写、块分配、inode 更新）
  - `os/fs/ext2_vfs.c`（VFS 文件读写操作）

### 3) 目录创建、查找与删除（2 pts）
- 已完成内容：实现目录项遍历、目录创建与删除、`getdents` 输出，支持从根目录开始的路径解析。
- 主要文件：
  - `os/fs/ext2_vfs.c`（目录项查找/插入/删除、getdents）

### 4) 直接与间接索引（2 pts）
- 已完成内容：实现 12 个直接块 + 一级/二级/三级间接块映射，支持大文件读写及索引测试。
- 主要文件：
  - `os/fs/ext2.c`（块映射与多级间接指针处理）

### 5) 元数据正确更新（1 pt）
- 已完成内容：写入/扩展时更新 inode size/nlinks，分配/回收时更新块位图与 inode 位图，保持全局计数一致。
- 主要文件：
  - `os/fs/ext2.c`（bitmap、inode 与超级块更新）

### 6) 接口规范与 FIFO（1 pt）
- 已完成内容：支持 `mkfifo` 与 FIFO 读写/关闭语义；VFS 打开时触发 FIFO 等待/唤醒逻辑。
- 主要文件：
  - `os/fs/ext2_vfs.c`（FIFO 实现）
  - `os/fs/vfs_syscall.c`（新增 inode `open` 回调）
  - `os/fs/fs.h`（inode_operations 增加 open 回调）

## Bonus（5 分）

### 7) 日志系统（1 pt）
- 状态：未实现（未引入 Ext3/Ext4 日志机制）。

### 8) 性能测试优化（4 pts）
- 状态：未实现（未做性能优化/区段等扩展）。

---

## 变更文件清单
- 新增：
  - `os/fs/ext2.h`
  - `os/fs/ext2.c`
  - `os/fs/ext2_vfs.c`
  - `EXT2_PROJECT_TASKS.md`
- 修改：
  - `os/fs/fs.h`
  - `os/fs/vfs_syscall.c`
