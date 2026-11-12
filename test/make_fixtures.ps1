# 生成测试样本：sample.txt（9 字节）与 large.bin（64 MiB，固定内容）。
# 参考值不在这里生成，改由 make_reference.py 用标准库独立计算。
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot

[IO.File]::WriteAllBytes("$dir\sample.txt", [Text.Encoding]::ASCII.GetBytes('123456789'))

# 固定内容的 64 MiB 样本，便于复现耗时与分块边界
$fs = [IO.File]::Create("$dir\large.bin")
try {
    $block = New-Object byte[] (1MB)
    for ($i = 0; $i -lt $block.Length; $i++) { $block[$i] = [byte](($i * 31 + 7) -band 0xFF) }
    for ($i = 0; $i -lt 64; $i++) { $fs.Write($block, 0, $block.Length) }
} finally {
    $fs.Close()
}

"wrote $dir\sample.txt and $dir\large.bin"
