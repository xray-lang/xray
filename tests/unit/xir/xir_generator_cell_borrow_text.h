/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_cell_borrow_text.h - One actual owned Cell and ref consumer program
 */
#ifndef XIR_GENERATOR_CELL_BORROW_TEXT_H
#define XIR_GENERATOR_CELL_BORROW_TEXT_H
static const char cell_borrow_text[]=
    "fn by_function(f:fn()->i64)->Iterator<i64>{try{yield f()} catch(error){yield -2}}\n"
    "fn advance(seed:ref i64,it:Iterator<i64>)->i64{try{return it.next()} catch(error){return -3}}\n"
    "fn owned_worker()->i64{var seed:i64=17\nconst f=fn()->i64{return seed}\n"
    "const it=by_function(f)\ntry{const value=it.next()\nreturn value} catch(error){return -99}}\n"
    "fn borrowed_worker()->i64{var seed:i64=17\nconst f=fn()->i64{return seed}\n"
    "const it=by_function(f)\nreturn advance(ref seed,it)}\n"
    "export fn run_owned()->i64{return await go owned_worker()}\n"
    "export fn run_borrowed()->i64{return await go borrowed_worker()}\nfn main(){}\n";
#endif // XIR_GENERATOR_CELL_BORROW_TEXT_H
