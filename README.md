# io_uring By Example

This repo contains my personal code implementation during the study of article series [io_uring By Example](https://unixism.net/2020/04/io-uring-by-example-article-series/) by [Shuveb Hussain](https://unixism.net/about-unixism/).

# Study Notes
I have written a series of study notes on [Zhihu](https://zhuanlan.zhihu.com/p/1929717237880723045) **(Chinese Simplified)**

# How to Use
## Compile
1. Install [liburing](https://github.com/axboe/liburing)
2. Copy one of the examples from the `examples/` folder into `main.c`.
3. Compile with `make`
4. Execute `./main`

## Format Source Code
`make clang-format`

## Code Style Check
`make clang-tidy`

## Clean-up
`make clean`