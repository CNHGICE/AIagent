#ifndef MATHPARSER_H
#define MATHPARSER_H

#include <QString>

#include <cmath>

// 简易递归下降求值器：支持 + - * / % ( ) 与一元正负号
class MathParser
{
public:
    explicit MathParser(const QString &text)
        : m_text(text)
        , m_pos(0)
    {
    }

    bool parse(double *out)
    {
        const bool ok = parseExpr(out);
        skipSpace();
        return ok && m_pos >= m_text.size();
    }

private:
    void skipSpace()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace())
            ++m_pos;
    }

    bool parseExpr(double *out)
    {
        if (!parseTerm(out))
            return false;
        for (;;) {
            skipSpace();
            if (m_pos >= m_text.size())
                return true;
            const QChar c = m_text.at(m_pos);
            if (c != QLatin1Char('+') && c != QLatin1Char('-'))
                return true;
            ++m_pos;
            double rhs = 0;
            if (!parseTerm(&rhs))
                return false;
            *out = (c == QLatin1Char('+')) ? *out + rhs : *out - rhs;
        }
    }

    bool parseTerm(double *out)
    {
        if (!parseFactor(out))
            return false;
        for (;;) {
            skipSpace();
            if (m_pos >= m_text.size())
                return true;
            const QChar c = m_text.at(m_pos);
            if (c != QLatin1Char('*') && c != QLatin1Char('/') && c != QLatin1Char('%'))
                return true;
            ++m_pos;
            double rhs = 0;
            if (!parseFactor(&rhs))
                return false;
            if (c == QLatin1Char('*')) {
                *out *= rhs;
            } else if (c == QLatin1Char('/')) {
                if (rhs == 0)
                    return false;
                *out /= rhs;
            } else {
                if (rhs == 0)
                    return false;
                *out = std::fmod(*out, rhs);
            }
        }
    }

    bool parseFactor(double *out)
    {
        skipSpace();
        if (m_pos >= m_text.size())
            return false;
        const QChar c = m_text.at(m_pos);
        if (c == QLatin1Char('(')) {
            ++m_pos;
            if (!parseExpr(out))
                return false;
            skipSpace();
            if (m_pos >= m_text.size() || m_text.at(m_pos) != QLatin1Char(')'))
                return false;
            ++m_pos;
            return true;
        }
        if (c == QLatin1Char('-')) {
            ++m_pos;
            if (!parseFactor(out))
                return false;
            *out = -*out;
            return true;
        }
        if (c == QLatin1Char('+')) {
            ++m_pos;
            return parseFactor(out);
        }
        const int start = m_pos;
        while (m_pos < m_text.size()
               && (m_text.at(m_pos).isDigit() || m_text.at(m_pos) == QLatin1Char('.')))
            ++m_pos;
        if (m_pos == start)
            return false;
        bool ok = false;
        const double value = m_text.mid(start, m_pos - start).toDouble(&ok);
        if (!ok)
            return false;
        *out = value;
        return true;
    }

    QString m_text;
    int m_pos;
};

#endif // MATHPARSER_H
