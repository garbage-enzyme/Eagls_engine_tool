import os
import sys
import argparse

# 解包器路径，需要根据实际编译后的位置修改
unpacker_path = r"..\x64\Release\pak_unpacker.exe"

def main():
    parser = argparse.ArgumentParser(description='解包EAGLS引擎的PAK文件')
    parser.add_argument('pak_file', help='PAK文件路径')
    parser.add_argument('--output', '-o', default='output', help='输出目录，默认为"output"')
    parser.add_argument('--no-decrypt', '-n', action='store_true', help='不解密文件内容')

    args = parser.parse_args()

    # 确保PAK文件存在
    if not os.path.exists(args.pak_file):
        print(f"错误: PAK文件不存在: {args.pak_file}")
        return 1

    # 确保输出目录存在
    os.makedirs(args.output, exist_ok=True)

    # 构建命令
    decrypt_flag = "0" if args.no_decrypt else "1"
    cmd = f'"{unpacker_path}" "{args.pak_file}" "{args.output}" {decrypt_flag}'

    print(f"执行命令: {cmd}")
    return os.system(cmd)

if __name__ == "__main__":
    sys.exit(main())
