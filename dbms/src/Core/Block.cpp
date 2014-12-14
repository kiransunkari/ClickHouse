#include <iterator>

#include <DB/Core/Exception.h>
#include <DB/Core/ErrorCodes.h>

#include <DB/Core/Block.h>

#include <DB/Storages/ColumnDefault.h>

#include <DB/Columns/ColumnArray.h>
#include <DB/DataTypes/DataTypeNested.h>

#include <DB/Parsers/ASTExpressionList.h>
#include <DB/Interpreters/ExpressionAnalyzer.h>
#include <statdaemons/ext/memory.hpp>

#include <DB/Parsers/formatAST.h>

namespace
{

bool typesAreCompatible(const DB::IDataType & lhs, const DB::IDataType & rhs)
{	
	if (lhs.behavesAsNumber() && rhs.behavesAsNumber())
		return true;
	
	if (lhs.behavesAsString() && rhs.behavesAsString())
		return true;
	
	if (lhs.getName() == rhs.getName())
		return true;

	return false;
}

}

namespace DB
{


Block::Block(const Block & other)
{
	*this = other;
}


void Block::addDefaults(const NamesAndTypesList & required_columns)
{
	for (const auto & column : required_columns)
		if (!has(column.name))
			insertDefault(column.name, column.type);
}

Block & Block::operator= (const Block & other)
{
	data = other.data;

	index_by_position.resize(data.size());
	index_by_name.clear();

	size_t pos = 0;
	for (Container_t::iterator it = data.begin(); it != data.end(); ++it, ++pos)
	{
		index_by_position[pos] = it;
		index_by_name[it->name] = it;
	}

	return *this;
}

void Block::insert(size_t position, const ColumnWithNameAndType & elem)
{
	if (position > index_by_position.size())
		throw Exception("Position out of bound in Block::insert(), max position = "
			+ toString(index_by_position.size()), ErrorCodes::POSITION_OUT_OF_BOUND);

	if (position == index_by_position.size())
	{
		insert(elem);
		return;
	}

	Container_t::iterator it = data.insert(index_by_position[position], elem);
	index_by_name[elem.name] = it;

	index_by_position.resize(index_by_position.size() + 1);
	for (size_t i = index_by_position.size() - 1; i > position; --i)
		index_by_position[i] = index_by_position[i - 1];

	index_by_position[position] = it;
}


void Block::insert(const ColumnWithNameAndType & elem)
{
	Container_t::iterator it = data.insert(data.end(), elem);
	index_by_name[elem.name] = it;
	index_by_position.push_back(it);
}

void Block::insertDefault(const String & name, const DataTypePtr & type)
{
	insert({
		dynamic_cast<IColumnConst &>(*type->createConstColumn(rows(),
			type->getDefault())).convertToFullColumn(),
		type, name
	});
}



void Block::insertUnique(const ColumnWithNameAndType & elem)
{
	if (index_by_name.end() == index_by_name.find(elem.name))
		insert(elem);
}


void Block::erase(size_t position)
{
	if (position >= index_by_position.size())
		throw Exception("Position out of bound in Block::erase(), max position = "
			+ toString(index_by_position.size()), ErrorCodes::POSITION_OUT_OF_BOUND);

	Container_t::iterator it = index_by_position[position];
	index_by_name.erase(index_by_name.find(it->name));
	data.erase(it);

	for (size_t i = position, size = index_by_position.size() - 1; i < size; ++i)
		index_by_position[i] = index_by_position[i + 1];

	index_by_position.resize(index_by_position.size() - 1);
}


void Block::erase(const String & name)
{
	IndexByName_t::iterator index_it = index_by_name.find(name);
	if (index_it == index_by_name.end())
		throw Exception("No such name in Block::erase(): '"
			+ name + "'", ErrorCodes::NOT_FOUND_COLUMN_IN_BLOCK);

	Container_t::iterator it = index_it->second;
	index_by_name.erase(index_it);
	size_t position = std::distance(data.begin(), it);
	data.erase(it);

	for (size_t i = position, size = index_by_position.size() - 1; i < size; ++i)
		index_by_position[i] = index_by_position[i + 1];

	index_by_position.resize(index_by_position.size() - 1);
}


ColumnWithNameAndType & Block::getByPosition(size_t position)
{
	if (position >= index_by_position.size())
		throw Exception("Position " + toString(position)
			+ " is out of bound in Block::getByPosition(), max position = "
			+ toString(index_by_position.size() - 1)
			+ ", there are columns: " + dumpNames(), ErrorCodes::POSITION_OUT_OF_BOUND);

	return *index_by_position[position];
}


const ColumnWithNameAndType & Block::getByPosition(size_t position) const
{
	if (position >= index_by_position.size())
		throw Exception("Position " + toString(position)
			+ " is out of bound in Block::getByPosition(), max position = "
			+ toString(index_by_position.size() - 1)
			+ ", there are columns: " + dumpNames(), ErrorCodes::POSITION_OUT_OF_BOUND);

	return *index_by_position[position];
}


ColumnWithNameAndType & Block::getByName(const std::string & name)
{
	IndexByName_t::const_iterator it = index_by_name.find(name);
	if (index_by_name.end() == it)
		throw Exception("Not found column " + name + " in block. There are only columns: " + dumpNames()
			, ErrorCodes::NOT_FOUND_COLUMN_IN_BLOCK);

	return *it->second;
}


const ColumnWithNameAndType & Block::getByName(const std::string & name) const
{
	IndexByName_t::const_iterator it = index_by_name.find(name);
	if (index_by_name.end() == it)
		throw Exception("Not found column " + name + " in block. There are only columns: " + dumpNames()
			, ErrorCodes::NOT_FOUND_COLUMN_IN_BLOCK);

	return *it->second;
}


bool Block::has(const std::string & name) const
{
	return index_by_name.end() != index_by_name.find(name);
}


size_t Block::getPositionByName(const std::string & name) const
{
	IndexByName_t::const_iterator it = index_by_name.find(name);
	if (index_by_name.end() == it)
		throw Exception("Not found column " + name + " in block. There are only columns: " + dumpNames()
			, ErrorCodes::NOT_FOUND_COLUMN_IN_BLOCK);

	return std::distance(const_cast<Container_t &>(data).begin(), it->second);
}


size_t Block::rows() const
{
	size_t res = 0;
	for (Container_t::const_iterator it = data.begin(); it != data.end(); ++it)
	{
		size_t size = it->column->size();

		if (res != 0 && size != res)
			throw Exception("Sizes of columns doesn't match: "
				+ data.begin()->name + ": " + toString(res)
				+ ", " + it->name + ": " + toString(size)
				, ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

		res = size;
	}

	return res;
}


size_t Block::rowsInFirstColumn() const
{
	if (data.empty())
		return 0;
	for (const ColumnWithNameAndType & column : data)
	{
		if (!column.column.isNull())
			return column.column->size();
	}

	return 0;
}


size_t Block::bytes() const
{
	size_t res = 0;
	for (const auto & col : data)
		res += col.column->byteSize();

	return res;
}


std::string Block::dumpNames() const
{
	std::stringstream res;
	for (Container_t::const_iterator it = data.begin(); it != data.end(); ++it)
	{
		if (it != data.begin())
			res << ", ";
		res << it->name;
	}
	return res.str();
}


std::string Block::dumpStructure() const
{
	std::stringstream res;
	for (Container_t::const_iterator it = data.begin(); it != data.end(); ++it)
	{
		if (it != data.begin())
			res << ", ";
		res << it->name << ' ' << it->type->getName() << ' ' << it->column->getName() << ' ' << it->column->size();
	}
	return res.str();
}


Block Block::cloneEmpty() const
{
	Block res;

	for (Container_t::const_iterator it = data.begin(); it != data.end(); ++it)
		res.insert(it->cloneEmpty());

	return res;
}


ColumnsWithNameAndType Block::getColumns() const
{
	return ColumnsWithNameAndType(data.begin(), data.end());
}


NamesAndTypesList Block::getColumnsList() const
{
	NamesAndTypesList res;

	for (Container_t::const_iterator it = data.begin(); it != data.end(); ++it)
		res.push_back(NameAndTypePair(it->name, it->type));

	return res;
}


void Block::checkNestedArraysOffsets() const
{
	/// Указатели на столбцы-массивы, для проверки равенства столбцов смещений во вложенных структурах данных
	typedef std::map<String, const ColumnArray *> ArrayColumns;
	ArrayColumns array_columns;

	for (Container_t::const_iterator it = data.begin(); it != data.end(); ++it)
	{
		const ColumnWithNameAndType & column = *it;

		if (const ColumnArray * column_array = typeid_cast<const ColumnArray *>(&*column.column))
		{
			String name = DataTypeNested::extractNestedTableName(column.name);

			ArrayColumns::const_iterator it = array_columns.find(name);
			if (array_columns.end() == it)
				array_columns[name] = column_array;
			else
			{
				if (!it->second->hasEqualOffsets(*column_array))
					throw Exception("Sizes of nested arrays do not match", ErrorCodes::SIZES_OF_ARRAYS_DOESNT_MATCH);
			}
		}
	}
}


void Block::optimizeNestedArraysOffsets()
{
	/// Указатели на столбцы-массивы, для проверки равенства столбцов смещений во вложенных структурах данных
	typedef std::map<String, ColumnArray *> ArrayColumns;
	ArrayColumns array_columns;

	for (Container_t::iterator it = data.begin(); it != data.end(); ++it)
	{
		ColumnWithNameAndType & column = *it;

		if (ColumnArray * column_array = typeid_cast<ColumnArray *>(&*column.column))
		{
			String name = DataTypeNested::extractNestedTableName(column.name);

			ArrayColumns::const_iterator it = array_columns.find(name);
			if (array_columns.end() == it)
				array_columns[name] = column_array;
			else
			{
				if (!it->second->hasEqualOffsets(*column_array))
					throw Exception("Sizes of nested arrays do not match", ErrorCodes::SIZES_OF_ARRAYS_DOESNT_MATCH);

				/// делаем так, чтобы столбцы смещений массивов внутри одной вложенной таблицы указывали в одно место
				column_array->getOffsetsColumn() = it->second->getOffsetsColumn();
			}
		}
	}
}


bool blocksHaveEqualStructure(const Block & lhs, const Block & rhs)
{
	size_t columns = lhs.columns();
	if (rhs.columns() != columns)
		return false;

	for (size_t i = 0; i < columns; ++i)
	{
		const IDataType & lhs_type = *lhs.getByPosition(i).type;
		const IDataType & rhs_type = *rhs.getByPosition(i).type;
		
		if (lhs_type.getName() != rhs_type.getName())
			return false;
	}

	return true;
}

bool blocksHaveCompatibleStructure(const Block & lhs, const Block & rhs)
{
	size_t columns = lhs.columns();
	if (rhs.columns() != columns)
		return false;

	for (size_t i = 0; i < columns; ++i)
	{
		const IDataType & lhs_type = *lhs.getByPosition(i).type;
		const IDataType & rhs_type = *rhs.getByPosition(i).type;
		
		if (!typesAreCompatible(lhs_type, rhs_type))
			return false;
	}

	return true;
}

void Block::clear()
{
	data.clear();
	index_by_name.clear();
	index_by_position.clear();
}

void Block::swap(Block & other)
{
	data.swap(other.data);
	index_by_name.swap(other.index_by_name);
	index_by_position.swap(other.index_by_position);
}

}
