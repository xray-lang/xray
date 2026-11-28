local n = @N@
local flags = {}
for i = 0, n do flags[i] = 1 end
local count, p = 0, 2
while p <= n do
  if flags[p] == 1 then
    count = count + 1
    local m = p * p
    while m <= n do flags[m] = 0; m = m + p end
  end
  p = p + 1
end
print(count)
