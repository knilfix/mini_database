def insert_sorted_list(list, last_element_index, value):
    temp = list[last_element_index]
    last_element_index -= 1
    while last_element_index >= 0 and list[last_element_index] > value:
        list[last_element_index + 1] = list[last_element_index]
        last_element_index -= 1

    list[last_element_index + 1] = value
    list.append(temp)
    return list


my_list = [10, 11, 12, 15, 16]
new_list = insert_sorted_list(my_list, len(my_list) - 1, 14)
print(new_list)
