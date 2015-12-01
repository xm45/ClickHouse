#include <DB/DataStreams/BlocksListBlockInputStream.h>
#include <DB/DataStreams/MergingAggregatedMemoryEfficientBlockInputStream.h>
#include <DB/DataStreams/AggregatingBlockInputStream.h>


namespace DB
{


Block AggregatingBlockInputStream::readImpl()
{
	if (!executed)
	{
		executed = true;
		AggregatedDataVariants data_variants;

		Aggregator::CancellationHook hook = [&]() { return this->isCancelled(); };
		aggregator.setCancellationHook(hook);

		aggregator.execute(children.back(), data_variants);

		if (!aggregator.hasTemporaryFiles())
		{
			impl.reset(new BlocksListBlockInputStream(
				aggregator.convertToBlocks(data_variants, final, 1)));
		}
		else
		{
			/** Если есть временные файлы с частично-агрегированными данными на диске,
			  *  то читаем и мерджим их, расходуя минимальное количество памяти.
			  */

			/// Сбросим имеющиеся в оперативке данные тоже на диск. Так проще.
			size_t rows = data_variants.sizeWithoutOverflowRow();
			if (rows)
				aggregator.writeToTemporaryFile(data_variants, rows);

			const auto & files = aggregator.getTemporaryFiles();
			BlockInputStreams input_streams;
			for (const auto & file : files.files)
			{
				temporary_inputs.emplace_back(new TemporaryFileStream(file->path()));
				input_streams.emplace_back(temporary_inputs.back()->block_in);
			}

			LOG_TRACE(log, "Will merge " << files.files.size() << " temporary files of size "
				<< (files.sum_size_compressed / 1048576.0) << " MiB compressed, "
				<< (files.sum_size_uncompressed / 1048576.0) << " MiB uncompressed.");

			impl.reset(new MergingAggregatedMemoryEfficientBlockInputStream(input_streams, params, final));
		}
	}

	Block res;
	if (isCancelled() || !impl)
		return res;

	return impl->read();
}


}
