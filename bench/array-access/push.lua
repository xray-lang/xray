local a = {}
for i = 0, 4999999 do a[#a + 1] = i end
local total = 0
for i = 1, #a do total = total + a[i] end
print(total)
