#include <iostream>
#include <thread>
#include <optional>

#include "boost/algorithm/string/classification.hpp"
#include "boost/algorithm/string/split.hpp"
#include "boost/lexical_cast.hpp"
#include "spdlog/spdlog.h"

#include "mio/mmap.hpp"

#include "mappable/mapper.hpp"
#include "index_types.hpp"
#include "wand_data_compressed.hpp"
#include "query/queries.hpp"
#include "util/util.hpp"

#include "cursor/cursor.hpp"
#include "cursor/scored_cursor.hpp"
#include "cursor/max_scored_cursor.hpp"

template <typename QueryOperator>
void op_profile(QueryOperator const& query_op,
                std::vector<pisa::term_id_vec> const& queries)
{
    using namespace pisa;

    size_t n_threads = std::thread::hardware_concurrency();
    std::vector<std::thread> threads(n_threads);
    std::mutex io_mutex;

    for (size_t tid = 0; tid < n_threads; ++tid) {
        threads[tid] = std::thread([&, tid]() {
                auto query_op_copy = query_op; // copy one query_op per thread
                for (size_t i = tid; i < queries.size(); i += n_threads) {
                    if (i % 10000 == 0) {
                        std::lock_guard<std::mutex> lock(io_mutex);
                        spdlog::info("{} queries processed", i);
                    }

                    query_op_copy(queries[i]);
                }
            });
    }

    for (auto& thread: threads) thread.join();
}

template <typename IndexType>
struct add_profiling { typedef IndexType type; };

template <typename BlockType>
struct add_profiling<pisa::block_freq_index<BlockType, false>> {
    typedef pisa::block_freq_index<BlockType, true> type;
};


template <typename IndexType>
void profile(const std::string index_filename,

             const std::optional<std::string> &wand_data_filename,
             std::vector<pisa::term_id_vec> const& queries,
             std::string const& type,
             std::string const& query_type)
{
    using namespace pisa;

    typename add_profiling<IndexType>::type index;
    typedef wand_data<bm25, wand_data_raw<bm25>> WandType;
    spdlog::info("Loading index from {}", index_filename);
    mio::mmap_source m(index_filename);
    mapper::map(index, m);

    WandType wdata;
    mio::mmap_source md;
    if (wand_data_filename) {
        std::error_code error;
        md.map(*wand_data_filename, error);
        if(error){
            std::cerr << "error mapping file: " << error.message() << ", exiting..." << std::endl;
            throw std::runtime_error("Error opening file");
        }
        mapper::map(wdata, md, mapper::map_flags::warmup);
    }

    spdlog::info("Performing {} queries", type);

    std::vector<std::string> query_types;
    boost::algorithm::split(query_types, query_type, boost::is_any_of(":"));

    for (auto const& t: query_types) {
        spdlog::info("Query type: {}", t);
        std::function<uint64_t(term_id_vec)> query_fun;
        if (t == "and") {
            query_fun = [&](term_id_vec terms){
                and_query<false> and_q(index.num_docs());
                return and_q(make_cursors<typename add_profiling<IndexType>::type>(index, terms));
            };
        } else if (t == "ranked_and" && wand_data_filename) {
            query_fun = [&](term_id_vec terms){
                ranked_and_query ranked_and_q(10, index.num_docs());
                return ranked_and_q(make_scored_cursors<typename add_profiling<IndexType>::type, WandType>(index, wdata, terms));
            };
        } else if (t == "wand" && wand_data_filename) {
            query_fun = [&](term_id_vec terms){
                wand_query wand_q(10, index.num_docs());
                return wand_q(make_max_scored_cursors<typename add_profiling<IndexType>::type, WandType>(index, wdata, terms));
            };
        } else if (t == "maxscore" && wand_data_filename) {
            query_fun = [&](term_id_vec terms){
                maxscore_query maxscore_q(10, index.num_docs());
                return maxscore_q(make_max_scored_cursors<typename add_profiling<IndexType>::type, WandType>(index, wdata, terms));
            };
        } else {
            spdlog::error("Unsupported query type: {}", t);
        }
        op_profile(query_fun, queries);
    }

    block_profiler::dump(std::cout);
}

int main(int argc, const char** argv)
{
    using namespace pisa;

    std::string type = argv[1];
    const char* query_type = argv[2];
    const char* index_filename = argv[3];
    std::optional<std::string> wand_data_filename;
    size_t args =4;
    if (argc > 4) {
        wand_data_filename = argv[4];
        args++;
    }

    std::vector<term_id_vec> queries;
    term_id_vec q;
    if (std::string(argv[args]) == "--file") {
        args++;
        args++;
        std::filebuf fb;
        if (fb.open(argv[args], std::ios::in)) {
            std::istream is(&fb);
            while (read_query(q, is)) queries.push_back(q);
        }
    } else {
        while (read_query(q)) queries.push_back(q);
    }

    if (false) {
#define LOOP_BODY(R, DATA, T)                                   \
        } else if (type == BOOST_PP_STRINGIZE(T)) {             \
            profile<BOOST_PP_CAT(T, _index)>                    \
                (index_filename, wand_data_filename, queries,   \
                 type, query_type);                             \
            /**/

        BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, PISA_INDEX_TYPES);
#undef LOOP_BODY
    } else {
        spdlog::error("Unknown type {}", type);
    }

}
