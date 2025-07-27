# io_uring By Example

My personal implementation of article series [io_uring By Example](https://unixism.net/2020/04/io-uring-by-example-article-series/) by [Shuveb Hussain](https://unixism.net/about-unixism/)

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

## Clean-up Binary
`make clean`