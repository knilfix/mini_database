import random


class BTreeNode:
    def __init__(self, min_degree, is_leaf) -> None:
        self.min_degree = min_degree
        self.max_keys = 2 * min_degree - 1

        self.keys = [None] * self.max_keys
        self.children = [None] * (self.max_keys + 1)
        self.num_keys = 0
        self.is_leaf = is_leaf

    def search(self, key):
        index = 0
        while index < self.num_keys and key > self.keys[index]:
            index += 1

        if index < self.num_keys and key == self.keys[index]:
            return self, index

        if self.is_leaf:
            return None, None

        return self.children[index].search(key)  # type: ignore

    def traverse(self):
        for index in range(self.num_keys):
            if not self.is_leaf:
                self.children[index].traverse()
            print(self.keys[index], end=" ")

        if not self.is_leaf:
            self.children[self.num_keys].traverse()

    def insert_non_full(self, key):
        index = self.num_keys - 1  # last key

        if self.is_leaf:
            # shift larger keys to the right, then drop the key into the gap
            while index >= 0 and self.keys[index] > key:
                self.keys[index + 1] = self.keys[index]
                index -= 1

            self.keys[index + 1] = key
            self.num_keys += 1

        else:
            # find which child the key belongs in
            while index >= 0 and self.keys[index] > key:
                index -= 1

            child_index = index + 1

            # if that child is full, split first
            if self.children[child_index].num_keys == self.max_keys:
                self.split_child(
                    child_index=child_index, full_child=self.children[child_index]
                )
                if self.keys[child_index] < key:
                    child_index += 1

            self.children[child_index].insert_non_full(key)

    def split_child(self, child_index, full_child):
        min_degree = self.min_degree

        # 1. Create the new right node, same level as the full child
        new_right_node = BTreeNode(min_degree=min_degree, is_leaf=full_child.is_leaf)
        new_right_node.num_keys = min_degree - 1

        # 2. Copy the upper keys into it
        for j in range(min_degree - 1):
            new_right_node.keys[j] = full_child.keys[j + min_degree]

        # 3. If internal , hand over the upper children too
        if not full_child.is_leaf:
            for j in range(min_degree):
                new_right_node.children[j] = full_child.children[j + min_degree]

        # 4. Shrink the old node (removes the moved keys)
        full_child.num_keys = min_degree - 1

        # 5. Make room in the parent's children and link the new node
        for j in range(self.num_keys, child_index, -1):
            self.children[j + 1] = self.children[j]
        self.children[child_index + 1] = new_right_node

        # 6. Make room in the parent's keys and insert the median
        for j in range(self.num_keys - 1, child_index - 1, -1):
            self.keys[j + 1] = self.keys[j]

        self.keys[child_index] = full_child.keys[min_degree - 1]
        self.num_keys += 1


class Btree:
    def __init__(self, min_degree) -> None:
        self.root = None
        self.min_degree = min_degree

    def insert(self, key):
        # Case 1 : empty tree, create the root as a leaf
        if self.root is None:
            self.root = BTreeNode(self.min_degree, True)
            self.root.keys[0] = key
            self.root.num_keys = 1
            return

        # Case 2 : Root is full , tree must grow taller
        if self.root.num_keys == self.root.max_keys:
            new_root = BTreeNode(self.min_degree, False)
            new_root.children[0] = self.root  # old root becomes child 0
            new_root.split_child(0, self.root)

            # new root has exactly 1 key, decide which half gets the key
            child_index = 0
            if new_root.keys[0] < key:
                child_index = 1

            new_root.children[child_index].insert_non_full(key)

            self.root = new_root

        # Case 3: Normal insert, root has room
        else:
            self.root.insert_non_full(key)

    def get(self, key):
        if self.root is None:
            return None
        node, index = self.root.search(key)
        if node is None:
            return None
        return node.keys[index]

    def traverse(self):
        if self.root is not None:
            self.root.traverse()


def show(node, depth=0):
    """Print the tree sideways: one node per line, indented by depth."""
    if node is None:
        return
    print("    " * depth + str(node.keys[: node.num_keys]))
    if not node.is_leaf:
        for i in range(node.num_keys + 1):
            show(node.children[i], depth + 1)


def main():
    b_tree = Btree(2)  # max 3 keys per node
    keys = [10, 20, 30, 40, 50, 25, 5, 35, 45, 60, 55]

    for key in keys:
        print(f"--- insert {key}")
        b_tree.insert(key)
        show(b_tree.root)


if __name__ == "__main__":
    main()
