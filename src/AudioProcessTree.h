#pragma once
#include <QMap>
#include <QSet>
#include <QString>

namespace AudioProcessTree {
struct Process {
  quint32 parent = 0;
  QString executable;
  quint64 created = 0;
};
using Processes = QMap<quint32, Process>;
inline quint32 parent(const Processes &processes, quint32 pid) {
  auto child = processes.constFind(pid);
  if (child == processes.cend())
    return 0;
  auto ancestor = processes.constFind(child->parent);
  // A recycled parent PID must never attach an application to another tree.
  if (ancestor == processes.cend() || ancestor->created >= child->created)
    return 0;
  return child->parent;
}
inline bool contains(const Processes &processes, quint32 root, quint32 pid) {
  for (; pid; pid = parent(processes, pid))
    if (pid == root)
      return true;
  return false;
}
inline bool containsExcludedApplication(const Processes &processes, quint32 root,
                                        const QSet<QString> &excluded) {
  for (auto it = processes.cbegin(); it != processes.cend(); ++it)
    if (excluded.contains(it->executable) && contains(processes, root, it.key()))
      return true;
  return false;
}
// Windows captures process trees. Collapse overlapping trees before presenting
// choices, so a selected parent cannot leak a separately excluded child.
inline QSet<quint32> roots(const Processes &processes,
                          const QSet<quint32> &sessions, quint32 self) {
  QSet<quint32> candidates;
  for (auto pid : sessions) {
    if (!processes.contains(pid))
      continue;
    auto root = pid;
    while (auto ancestor = parent(processes, root)) {
      if (processes[ancestor].executable != processes[root].executable)
        break;
      root = ancestor;
    }
    candidates.insert(root);
  }
  auto result = candidates;
  for (auto root : candidates) {
    if (contains(processes, root, self)) {
      result.remove(root);
      continue;
    }
    for (auto ancestor : candidates)
      if (root != ancestor && contains(processes, ancestor, root)) {
        result.remove(root);
        break;
      }
  }
  return result;
}
} // namespace AudioProcessTree
