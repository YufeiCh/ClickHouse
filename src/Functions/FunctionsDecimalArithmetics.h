#pragma once
#include <type_traits>
#include <Core/AccurateComparison.h>

#include <DataTypes/DataTypesDecimal.h>
#include <Columns/ColumnsNumber.h>
#include <Functions/IFunction.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/castTypeToEither.h>

#include <IO/WriteHelpers.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int DECIMAL_OVERFLOW;
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}


struct DecimalOpHerpers
{
    static std::vector<UInt8> multiply(std::vector<UInt8> num1, std::vector<UInt8> num2)
    {
        int len1 = num1.size();
        int len2 = num2.size();
        if (len1 == 0 || len2 == 0)
            return {0};

        std::vector<UInt8> result(len1 + len2, 0);
        int i_n1 = 0;
        int i_n2;

        for (int i = len1 - 1; i >= 0; --i)
        {
            int carry = 0;
            int n1 = num1[i];
            i_n2 = 0;
            for (int j = len2 - 1; j >= 0; --j)
            {
                int n2 = num2[j];
                int sum = n1 * n2 + result[i_n1 + i_n2] + carry;
                carry = sum / 10;
                result[i_n1 + i_n2] = sum % 10;
                ++i_n2;
            }

            if (carry > 0)
                result[i_n1 + i_n2] += carry;

            ++i_n1;
        }

        int i = result.size() - 1;
        while (i >= 0 && result[i] == 0)
            --i;
        if (i == -1)
            return {0};

        std::reverse(result.begin(), result.end());
        return result;
    }

    static std::vector<UInt8> divide(std::vector<UInt8> number, Int256 divisor)
    {
        std::vector<UInt8> ans;

        int idx = 0;
        int temp = number[idx];
        while (temp < divisor)
            temp = temp * 10 + (number[++idx]);

        while (int(number.size()) > idx) {
            ans.push_back(temp / divisor);
            temp = (temp % divisor) * 10 + number[++idx];
        }

        if (ans.empty())
            return {0};

        return ans;
    }

    static std::vector<UInt8> getDigits(Int256 x)
    {
        std::vector<UInt8> result;
        if (x >= 10)
            result = getDigits(x / 10);

        result.push_back(x % 10);
        return result;
    }

    static Int256 fromDigits(std::vector<UInt8> digits) {
        Int256 result = 0;
        Int256 multiplier = 1;
        for (auto i = digits.rbegin(); i != digits.rend(); ++i ) {
            result += multiplier * (*i);
            multiplier *= 10;
        }
        return result;
    }
};


struct DivideDecimalsImpl
{
    static constexpr auto name = "divideDecimal";

    template <typename FirstType, typename SecondType>
    static inline NO_SANITIZE_UNDEFINED Decimal256
    execute(FirstType a, SecondType b, UInt16 scale_a, UInt16 scale_b, UInt16 result_scale)
    {
        Int8 sign_a = a.value < 0 ? -1 : 1;
        Int8 sign_b = b.value < 0 ? -1 : 1;
        std::vector<UInt8> a_digits = DecimalOpHerpers::getDigits(a.value * sign_a);

        for (int i = 0; i < scale_b - scale_a; ++i)
        {
            a_digits.push_back(0);
        }

        for (int i = 0; i < result_scale; ++i)
        {
            a_digits.push_back(0);
        }

        auto divided = DecimalOpHerpers::divide(a_digits, b.value);
        if (divided.size() > 76)
            throw DB::Exception("Numeric overflow", ErrorCodes::DECIMAL_OVERFLOW);

        return Decimal256(sign_a * sign_b * DecimalOpHerpers::fromDigits(divided));
    }
};

struct MultiplyDecimalsImpl
{
    static constexpr auto name = "multiplyDecimal";

    template <typename FirstType, typename SecondType>
    static inline NO_SANITIZE_UNDEFINED Decimal256
    execute(FirstType a, SecondType b, UInt16 scale_a, UInt16 scale_b, UInt16 result_scale)
    {
        Int8 sign_a = a.value < 0 ? -1 : 1;
        Int8 sign_b = b.value < 0 ? -1 : 1;
        std::vector<UInt8> a_digits = DecimalOpHerpers::getDigits(a.value * sign_a);
        std::vector<UInt8> b_digits = DecimalOpHerpers::getDigits(b.value * sign_b);

        auto multiplied = DecimalOpHerpers::multiply(a_digits, b_digits);

        if (result_scale > scale_a + scale_b)
            for (UInt8 i = 0; i < result_scale - (scale_a + scale_b); ++i)
                multiplied.push_back(0);

        if (result_scale < scale_a + scale_b)
            for (UInt8 i = 0; i < scale_a + scale_b - result_scale; ++i)
                multiplied.pop_back();

        if (multiplied.size() > 76)
            throw DB::Exception("Numeric overflow", ErrorCodes::DECIMAL_OVERFLOW);

        return Decimal256(sign_a * sign_b * DecimalOpHerpers::fromDigits(multiplied));
    }
};


template <typename ResultType, typename Transform>
struct Processor
{
    const Transform transform;

    explicit Processor(Transform transform_)
        : transform(std::move(transform_))
    {}

    template <typename FirstArgVectorType, typename SecondArgType>
    void NO_INLINE
    vectorConstant(const FirstArgVectorType & vec_first, const SecondArgType second_value,
                   PaddedPODArray<typename ResultType::FieldType> & vec_to, UInt16 scale_a, UInt16 scale_b, UInt16 result_scale) const
    {
        size_t size = vec_first.size();
        vec_to.resize(size);

        for (size_t i = 0; i < size; ++i)
            vec_to[i] = transform.execute(vec_first[i], second_value, scale_a, scale_b, result_scale);
    }

    template <typename FirstArgVectorType, typename SecondArgVectorType>
    void NO_INLINE NO_SANITIZE_UNDEFINED
    vectorVector(const FirstArgVectorType & vec_first, const SecondArgVectorType & vec_second,
                 PaddedPODArray<typename ResultType::FieldType> & vec_to, UInt16 scale_a, UInt16 scale_b, UInt16 result_scale) const
    {
        size_t size = vec_first.size();
        vec_to.resize(size);

        for (size_t i = 0; i < size; ++i)
            vec_to[i] = transform.execute(vec_first[i], vec_second[i], scale_a, scale_b, result_scale);
    }

    template <typename FirstArgType, typename SecondArgVectorType>
    void NO_INLINE NO_SANITIZE_UNDEFINED
    constantVector(const FirstArgType & first_value, const SecondArgVectorType & vec_second,
                   PaddedPODArray<typename ResultType::FieldType> & vec_to, UInt16 scale_a, UInt16 scale_b, UInt16 result_scale) const
    {
        size_t size = vec_second.size();
        vec_to.resize(size);

        for (size_t i = 0; i < size; ++i)
            vec_to[i] = transform.execute(first_value, vec_second[i], scale_a, scale_b, result_scale);
    }
};


template <typename FirstArgType, typename SecondArgType, typename ResultType, typename Transform>
struct DecimalArithmeticsImpl
{
    static ColumnPtr execute(Transform transform, const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type)
    {
        using FirstArgValueType = typename FirstArgType::FieldType;
        using FirstArgColumnType = typename FirstArgType::ColumnType;
        using SecondArgValueType = typename SecondArgType::FieldType;
        using SecondArgColumnType = typename SecondArgType::ColumnType;
        using ResultColumnType = typename ResultType::ColumnType;

        UInt16 scale_a = getDecimalScale(*arguments[0].type);
        UInt16 scale_b = getDecimalScale(*arguments[1].type);
        UInt16 result_scale = getDecimalScale(*result_type->getPtr());

        auto op = Processor<ResultType, Transform>{std::move(transform)};

        auto result_col = result_type->createColumn();
        auto col_to = assert_cast<ResultColumnType *>(result_col.get());

        const auto * first_col = checkAndGetColumn<FirstArgColumnType>(arguments[0].column.get());
        const auto * second_col = checkAndGetColumn<SecondArgColumnType>(arguments[1].column.get());
        const auto * first_col_const = typeid_cast<const ColumnConst *>(arguments[0].column.get());
        const auto * second_col_const = typeid_cast<const ColumnConst *>(arguments[1].column.get());

        if (first_col)
        {
            if (second_col_const)
                op.vectorConstant(first_col->getData(), second_col_const->template getValue<SecondArgValueType>(), col_to->getData(), scale_a, scale_b, result_scale);
            else
                op.vectorVector(first_col->getData(), second_col->getData(), col_to->getData(), scale_a, scale_b, result_scale);
        }
        else if (first_col_const)
        {
            op.constantVector(first_col_const->template getValue<FirstArgValueType>(), second_col->getData(), col_to->getData(), scale_a, scale_b, result_scale);
        }
        else
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of first argument of function {}",
                            arguments[0].column->getName(), Transform::name);
        }

        return result_col;
    }
};



template <typename Transform>
class FunctionsDecimalArithmetics : public IFunction
{
public:
    static constexpr auto name = Transform::name;
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionsDecimalArithmetics>(); }

    String getName() const override
    {
        return name;
    }

    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() != 2 && arguments.size() != 3) {
            throw Exception("Number of arguments for function " + getName() + " does not match: 2 or 3 expected",
                            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
        }

        if (!isDecimal(arguments[0].type) || !isDecimal(arguments[1].type))
            throw Exception("Arguments for " + getName() + " function must be Decimal", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

        if (arguments.size() == 3 && !isUnsignedInteger(arguments[2].type))
            throw Exception{"Illegal type " + arguments[2].type->getName() + " of third argument of function " + getName() +
                                    ". Should be constant Integer from range[0, 76]", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        UInt16 scale = arguments.size() == 3 ? checkAndGetColumnConst<ColumnUInt16>(arguments[2].column.get())->getValue<UInt16>() :
                                             std::max(getDecimalScale(*arguments[0].type->getPtr()), getDecimalScale(*arguments[1].type->getPtr()));

        /**
        At compile time, result is unknown. We only know the Scale (number of fractional digits) at runtime.
        Also nothing is known about size of whole part.
        As in simple division/multiplication for decimals, we scale the result up, but is is explicit here and no downscale is performed.
        It guarantees that result will have given scale and it can also be MANUALLY converted to other decimal types later.
        **/
        if (scale <= 76)
            return std::make_shared<DataTypeDecimal256>(76, scale);

        throw Exception{
            "Illegal value of third argument of function " + this->getName() + ": must be integer in range [0, 76]",
            ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT
        };
    }

    bool useDefaultImplementationForConstants() const override { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {2}; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t /*input_rows_count*/) const override
    {
        return resolveOverload(arguments, result_type);
    }

private:
    //long resolver to call proper templated func
    ColumnPtr resolveOverload(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type) const
    {
        WhichDataType which_divident(arguments[0].type.get());
        WhichDataType which_divisor(arguments[1].type.get());
        if (which_divident.isDecimal32())
        {
            using DividentType = DataTypeDecimal32;
            if (which_divisor.isDecimal32())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal32, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal64())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal64, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal128())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal128, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal256())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal256, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
        }

        else if (which_divident.isDecimal64())
        {
            using DividentType = DataTypeDecimal64;
            if (which_divisor.isDecimal32())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal32, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal64())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal64, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal128())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal128, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal256())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal256, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);

        }

        else if (which_divident.isDecimal128())
        {
            using DividentType = DataTypeDecimal128;
            if (which_divisor.isDecimal32())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal32, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal64())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal64, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal128())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal128, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal256())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal256, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);

        }

        else if (which_divident.isDecimal256())
        {
            using DividentType = DataTypeDecimal256;
            if (which_divisor.isDecimal32())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal32, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal64())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal64, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal128())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal128, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);
            else if (which_divisor.isDecimal256())
                return DecimalArithmeticsImpl<DividentType, DataTypeDecimal256, DataTypeDecimal256, Transform>::execute(Transform{}, arguments, result_type);

        }

        return nullptr;
    }
};

}

