def main():
    a = []
    for i in range(5000000):
        a.append(i)
    total = 0
    for i in range(len(a)):
        total += a[i]
    print(total)
main()
