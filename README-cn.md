### QEMU 自述文件

QEMU是一个通用的开源机器和用户空间模拟器和虚拟器。

QEMU 能够在软件中模拟整台机器，而无需任何硬件虚拟化支持。通过使用动态翻译，它实现了非常好的性能。QEMU 还可以与 Xen 和 KVM 虚拟机管理程序集成，以提供模拟硬件，同时允许虚拟机管理程序管理 CPU。借助虚拟机管理程序支持，QEMU 可以为 CPU 实现接近本机的性能。当 QEMU 直接模拟 CPU 时，它能够在另一台机器（例如 x86_64 PC 板）上运行为一台机器（例如 ARMv7 板）制作的操作系统。

QEMU 还能够为 Linux 和 BSD 内核接口提供用户空间 API 虚拟化。这允许针对一种架构 ABI（例如 Linux PPC64 ABI）编译的二进制文件在使用不同架构 ABI（例如 Linux x86_64 ABI）的主机上运行。这不涉及任何硬件仿真，只需 CPU 和系统调用仿真。

QEMU 旨在适应各种用例。希望完全控制其行为和设置的用户可以直接调用它。它还旨在通过提供稳定的命令行界面和监控 API 来促进与更高级别管理层的集成。当使用 oVirt、OpenStack 和 virt-manager 等开源应用程序时，它通常通过 libvirt 库间接调用。

QEMU 作为一个整体在 GNU 通用公共许可证第 2 版下发布。有关完整的许可详细信息，请参阅 LICENSE 文件。

#### 文档

文档可以在 https://www.qemu.org/documentation/ 在线找到。当前开发版本的文档可从 https://www.qemu.org/docs/master/ 获得，该文档是从源代码树中的 docs/ 文件夹生成的，由 Sphinx _ 构建。

#### 建筑

QEMU 是多平台软件，旨在在所有现代 Linux 平台、OS-X、Win32（通过 Mingw64 工具链）和各种其他 UNIX 目标上构建。构建 QEMU 的简单步骤如下：

       mkdir build
       cd build
       ../configure
       make

其他信息也可通过 QEMU 网站在线找到：

   + https://wiki.qemu.org/Hosts/Linux
   + https://wiki.qemu.org/Hosts/Mac
   + https://wiki.qemu.org/Hosts/W32

#### 提交补丁

QEMU 源代码在 GIT 版本控制系统下维护。

     git clone https://gitlab.com/qemu-project/qemu.git

提交补丁时，一种常见的方法是使用“git format-patch”和/或“git send-email”来格式化邮件并将其发送到 qemu-devel@nongnu.org 邮件列表。所有提交的补丁都必须包含作者的“Signed-off-by”行。补丁应遵循《开发人员指南》样式部分中规定的准则。

有关提交补丁的更多信息，请访问QEMU网站

  +  https://wiki.qemu.org/Contribute/SubmitAPatch
  +  https://wiki.qemu.org/Contribute/TrivialPatches

QEMU 网站也在源代码管理下进行维护。

     git clone https://gitlab.com/qemu-project/qemu-web.git

  +  https://www.qemu.org/2017/02/04/the-new-qemu-website-is-up/

创建了一个“git-publish”实用程序，以使上述过程不那么繁琐，强烈建议用于定期贡献，甚至只是发送连续的补丁系列修订。它还需要一个有效的“git send-email”设置，并且默认情况下不会自动执行所有操作，因此您可能需要手动完成上述步骤一次。

有关安装说明，请转到

  +  https://github.com/stefanha/git-publish

“git-publish”的工作流程是：

     $ git checkout master -b my-feature
     $ # work on new commits, add your 'Signed-off-by' lines to each
     $ git publish

您的补丁系列将被发送并标记为 my-feature-v1，如果您将来需要参考它。

  Sending v2: 发送 v2：

     $ git checkout my-feature # same topic branch
     $ # making changes to the commits (using 'git rebase', for example)
     $ git publish

您的补丁系列将在主题中带有“v2”标签，而 git 提示将被标记为 my-feature-v2。

#### 错误报告

QEMU 项目使用 GitLab 问题来跟踪错误。在运行从 QEMU git 或上游发布源代码构建的代码时发现的错误应通过以下方式报告：

  +  https://gitlab.com/qemu-project/qemu/-/issues

#### 更改日志

有关版本历史记录和发行说明，请访问 https://wiki.qemu.org/ChangeLog/ 或查看git历史记录以获取更多详细信息。

#### 联系

可以通过多种方式联系 QEMU 社区，主要有两种方式是电子邮件和 IRC

  +  mailto:qemu-devel@nongnu.org
  +  https://lists.nongnu.org/mailman/listinfo/qemu-devel
  +  #qemu on irc.oftc.net #qemu irc.oftc.net

有关联系社区的其他方法的信息，请访问QEMU网站：

  +  https://wiki.qemu.org/Contribute/StartHere

