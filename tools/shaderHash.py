import sys

def at_string_hash(s: str, init_value: int = 0) -> int:
    def u32(x):
        return x & 0xFFFFFFFF

    i = 0
    quotes = (s.startswith('"'))
    key = u32(init_value)

    if quotes:
        i += 1

    while i < len(s) and (not quotes or s[i] != '"'):
        ch = ord(s[i])
        i += 1

        # 'A'..'Z' → lowercase
        if ord('A') <= ch <= ord('Z'):
            ch += ord('a') - ord('A')
        elif ch == ord('\\'):
            ch = ord('/')

        key = u32(key + ch)
        key = u32(key + (key << 10))
        key = u32(key ^ (key >> 6))

    key = u32(key + (key << 3))
    key = u32(key ^ (key >> 11))
    key = u32(key + (key << 15))

    return key


def main():
    if len(sys.argv) < 2:
        print("usage: at_string_hash_jenkins.py <string> [init_value]")
        sys.exit(1)

    s = sys.argv[1]
    init_value = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0

    result = at_string_hash(s, init_value)

    print(f"hash: {result}")
    print(f"hex : 0x{result:08X}")


if __name__ == "__main__":
    main()
