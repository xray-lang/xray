local a = {}
for i = 0, 4095 do a[i + 1] = i end
for r = 1, @R@ do
  for i = 1, #a do a[i] = a[i] * 3 + 1 end
end
local total = 0
for i = 1, #a do total = total + a[i] end
print(total)
