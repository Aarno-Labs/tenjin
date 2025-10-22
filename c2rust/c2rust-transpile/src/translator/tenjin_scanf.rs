#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Directive {
    ZeroOrMoreWhitespace,
    OrdinaryChar(char),
    ConversionSpec(ConvSpec),
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ConvSpec {
    nth: Option<usize>,
    suppress_assignment: bool, // *
    thousands_separator: bool, // '
    implicit_allocation: bool, // m
    width: Option<usize>,
    type_modifier: Option<(char, char)>, // h, hh, j, l, ll, L, q, t, z
    specifier: char,                     // %, d, i, o, u, x, X, f, e, E, g, s, c, [, p, n
    bracket_content: Option<String>,     // for %[...] conversion
}

pub fn directive_is_simple(directive: &Directive) -> bool {
    match directive {
        Directive::ZeroOrMoreWhitespace => true,
        Directive::OrdinaryChar(_) => true,
        Directive::ConversionSpec(spec) => {
            // We don't care about type modifiers, which are not needed
            // given Rust's stronger type system.
            // For now, we consider specifiers c and s as not simple,
            // since these will correspond to unsafe pointers, sans guidance.
            spec.nth.is_none()
                && !spec.suppress_assignment
                && !spec.thousands_separator
                && !spec.implicit_allocation
                && spec.width.is_none()
                && spec.bracket_content.is_none()
                && !"o[pnsc".contains(spec.specifier)
        }
    }
}

pub fn parse_scanf_format(fmt: &str) -> Result<Vec<Directive>, String> {
    let mut directives = Vec::new();
    let mut chars = fmt.chars().peekable();

    while let Some(ch) = chars.next() {
        match ch {
            // Whitespace directive - any sequence of whitespace becomes one directive
            ch if ch.is_whitespace() => {
                // Skip any additional whitespace characters
                while let Some(&next_ch) = chars.peek() {
                    if next_ch.is_whitespace() {
                        chars.next();
                    } else {
                        break;
                    }
                }
                directives.push(Directive::ZeroOrMoreWhitespace);
            }

            // Conversion specification
            '%' => {
                let conv_spec = parse_conversion_spec(&mut chars)?;
                if conv_spec.specifier == '%' {
                    // If the specifier is '%', we treat it as an ordinary character
                    directives.push(Directive::OrdinaryChar('%'));
                } else {
                    // Otherwise, we add the conversion specification
                    directives.push(Directive::ConversionSpec(conv_spec));
                }
            }

            // Ordinary character
            ordinary => {
                directives.push(Directive::OrdinaryChar(ordinary));
            }
        }
    }

    Ok(directives)
}

fn parse_conversion_spec(
    chars: &mut std::iter::Peekable<std::str::Chars>,
) -> Result<ConvSpec, String> {
    let mut nth = -1;
    let mut suppress_assignment = false;
    let mut thousands_separator = false;
    let mut implicit_allocation = false;
    let mut width = None;
    let mut type_modifier = None;

    // Check for positional parameter "%n$"
    if let Some(&ch) = chars.peek() {
        if ch.is_ascii_digit() {
            let mut pos_str = String::new();
            while let Some(&ch) = chars.peek() {
                if ch.is_ascii_digit() {
                    pos_str.push(chars.next().unwrap());
                } else {
                    break;
                }
            }

            if chars.peek() == Some(&'$') {
                chars.next(); // consume '$'
                nth = pos_str
                    .parse()
                    .map_err(|_| "Invalid positional parameter")?;
            } else {
                // Put the digits back by creating a new iterator
                // This is a bit tricky - we need to handle this case
                // For now, we'll parse width later if we encounter digits
                return parse_conversion_spec_with_prefix(chars, &pos_str);
            }
        }
    }

    // Parse flags in order: *, ', m
    loop {
        match chars.peek() {
            Some('*') => {
                if suppress_assignment {
                    return Err("Duplicate assignment suppression flag".to_string());
                }
                suppress_assignment = true;
                chars.next();
            }
            Some('\'') => {
                if thousands_separator {
                    return Err("Duplicate thousands separator flag".to_string());
                }
                thousands_separator = true;
                chars.next();
            }
            Some('m') => {
                if implicit_allocation {
                    return Err("Duplicate implicit allocation flag".to_string());
                }
                implicit_allocation = true;
                chars.next();
            }
            _ => break,
        }
    }

    // Parse width
    if let Some(&ch) = chars.peek() {
        if ch.is_ascii_digit() {
            let mut width_str = String::new();
            while let Some(&ch) = chars.peek() {
                if ch.is_ascii_digit() {
                    width_str.push(chars.next().unwrap());
                } else {
                    break;
                }
            }
            width = Some(width_str.parse().map_err(|_| "Invalid width specifier")?);
        }
    }

    // Parse type modifier
    if let Some(&ch) = chars.peek() {
        match ch {
            'h' => {
                chars.next();
                if chars.peek() == Some(&'h') {
                    chars.next();
                    type_modifier = Some(('h', 'h'));
                } else {
                    type_modifier = Some(('h', '\0'));
                }
            }
            'l' => {
                chars.next();
                if chars.peek() == Some(&'l') {
                    chars.next();
                    type_modifier = Some(('l', 'l'));
                } else {
                    type_modifier = Some(('l', '\0'));
                }
            }
            'j' | 'L' | 'q' | 't' | 'z' => {
                type_modifier = Some((chars.next().unwrap(), '\0'));
            }
            _ => {}
        }
    }

    // Parse conversion specifier
    let specifier = chars.next().ok_or("Missing conversion specifier")?;

    let mut bracket_content = String::new();

    // Validate the specifier
    match specifier {
        '%' | 'd' | 'i' | 'o' | 'u' | 'x' | 'X' | 'f' | 'e' | 'E' | 'g' | 'a' | 's' | 'c' | 'p'
        | 'n' => {}
        '[' => {
            // For bracket expressions, we need to consume until the closing ]
            // This is complex because ] can be part of the set if it's the first character
            let mut first_char = true;

            for ch in chars.by_ref() {
                if ch == ']' && !first_char {
                    break;
                } else if ch == ']' && first_char {
                    bracket_content.push(ch);
                }
                bracket_content.push(ch);
                first_char = false;
            }

            if bracket_content.is_empty() {
                return Err("Empty bracket expression".to_string());
            }
        }
        _ => return Err(format!("Invalid conversion specifier: {specifier}")),
    }

    // Validate flag combinations
    if implicit_allocation && !matches!(specifier, 's' | 'c' | '[') {
        return Err("Implicit allocation (m) can only be used with %s, %c, or %[".to_string());
    }

    if thousands_separator && !matches!(specifier, 'd' | 'i' | 'o' | 'u' | 'x' | 'X') {
        return Err(
            "Thousands separator (') can only be used with decimal conversions".to_string(),
        );
    }

    let nth = if nth < 0 { None } else { Some(nth as usize) };

    let bracket_content: Option<String> = if bracket_content.is_empty() {
        None
    } else {
        Some(bracket_content)
    };

    Ok(ConvSpec {
        nth,
        suppress_assignment,
        thousands_separator,
        implicit_allocation,
        width,
        type_modifier,
        specifier,
        bracket_content,
    })
}

fn parse_conversion_spec_with_prefix(
    chars: &mut std::iter::Peekable<std::str::Chars>,
    prefix: &str,
) -> Result<ConvSpec, String> {
    // This handles the case where we started reading digits thinking it was a positional parameter
    // but it turned out to be a width specifier
    let mut suppress_assignment = false;
    let mut thousands_separator = false;
    let mut implicit_allocation = false;
    let mut type_modifier = None;

    // Parse remaining flags
    loop {
        match chars.peek() {
            Some('*') => {
                if suppress_assignment {
                    return Err("Duplicate assignment suppression flag".to_string());
                }
                suppress_assignment = true;
                chars.next();
            }
            Some('\'') => {
                if thousands_separator {
                    return Err("Duplicate thousands separator flag".to_string());
                }
                thousands_separator = true;
                chars.next();
            }
            Some('m') => {
                if implicit_allocation {
                    return Err("Duplicate implicit allocation flag".to_string());
                }
                implicit_allocation = true;
                chars.next();
            }
            _ => break,
        }
    }

    // The prefix is our width
    let width = prefix.parse().map_err(|_| "Invalid width specifier")?;

    // Parse additional width digits if any
    let mut final_width = width;
    if let Some(&ch) = chars.peek() {
        if ch.is_ascii_digit() {
            let mut width_str = prefix.to_string();
            while let Some(&ch) = chars.peek() {
                if ch.is_ascii_digit() {
                    width_str.push(chars.next().unwrap());
                } else {
                    break;
                }
            }
            final_width = width_str.parse().map_err(|_| "Invalid width specifier")?;
        }
    }

    // Parse type modifier
    if let Some(&ch) = chars.peek() {
        match ch {
            'h' => {
                chars.next();
                if chars.peek() == Some(&'h') {
                    chars.next();
                    type_modifier = Some(('h', 'h'));
                } else {
                    type_modifier = Some(('h', '\0'));
                }
            }
            'l' => {
                chars.next();
                if chars.peek() == Some(&'l') {
                    chars.next();
                    type_modifier = Some(('l', 'l'));
                } else {
                    type_modifier = Some(('l', '\0'));
                }
            }
            'j' | 'L' | 'q' | 't' | 'z' => {
                type_modifier = Some((chars.next().unwrap(), '\0'));
            }
            _ => {}
        }
    }

    // Parse conversion specifier
    let specifier = chars.next().ok_or("Missing conversion specifier")?;

    let mut bracket_content = String::new();

    // Validate the specifier (same as before)
    match specifier {
        '%' | 'd' | 'i' | 'o' | 'u' | 'x' | 'X' | 'f' | 'e' | 'E' | 'g' | 'a' | 's' | 'c' | 'p'
        | 'n' => {}
        '[' => {
            let mut first_char = true;

            for ch in chars.by_ref() {
                if ch == ']' && !first_char {
                    break;
                } else if ch == ']' && first_char {
                    bracket_content.push(ch);
                }
                bracket_content.push(ch);
                first_char = false;
            }

            if bracket_content.is_empty() {
                return Err("Empty bracket expression".to_string());
            }
        }
        _ => return Err(format!("Invalid conversion specifier: {specifier}")),
    }

    // Validate flag combinations
    if implicit_allocation && !matches!(specifier, 's' | 'c' | '[') {
        return Err("Implicit allocation (m) can only be used with %s, %c, or %[".to_string());
    }

    if thousands_separator && !matches!(specifier, 'd' | 'i' | 'o' | 'u' | 'x' | 'X') {
        return Err(
            "Thousands separator (') can only be used with decimal conversions".to_string(),
        );
    }

    let bracket_content: Option<String> = if bracket_content.is_empty() {
        None
    } else {
        Some(bracket_content)
    };

    Ok(ConvSpec {
        nth: None,
        suppress_assignment,
        thousands_separator,
        implicit_allocation,
        width: Some(final_width),
        type_modifier,
        specifier,
        bracket_content,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_simple_format() {
        let result = parse_scanf_format("%d").unwrap();
        assert_eq!(result.len(), 1);
        match &result[0] {
            Directive::ConversionSpec(spec) => {
                assert_eq!(spec.specifier, 'd');
                assert!(!spec.suppress_assignment);
                assert_eq!(spec.nth, None);
            }
            _ => panic!("Expected ConversionSpec"),
        }
    }

    #[test]
    fn test_simple_format_twice() {
        let result = parse_scanf_format("%d %u").unwrap();
        assert_eq!(result.len(), 3);
        match &result[0] {
            Directive::ConversionSpec(spec) => {
                assert_eq!(spec.specifier, 'd');
                assert!(!spec.suppress_assignment);
                assert_eq!(spec.nth, None);
            }
            _ => panic!("Expected ConversionSpec"),
        }
        match &result[1] {
            Directive::ZeroOrMoreWhitespace => {}
            _ => panic!("Expected ZeroOrMoreWhitespace"),
        }
        match &result[2] {
            Directive::ConversionSpec(spec) => {
                assert_eq!(spec.specifier, 'u');
                assert!(!spec.suppress_assignment);
                assert_eq!(spec.nth, None);
            }
            _ => panic!("Expected ConversionSpec"),
        }
    }

    #[test]
    fn test_whitespace() {
        let result = parse_scanf_format("  \t\n  ").unwrap();
        assert_eq!(result.len(), 1);
        assert_eq!(result[0], Directive::ZeroOrMoreWhitespace);
    }

    #[test]
    fn test_ordinary_chars() {
        let result = parse_scanf_format("abc").unwrap();
        assert_eq!(result.len(), 3);
        assert_eq!(result[0], Directive::OrdinaryChar('a'));
        assert_eq!(result[1], Directive::OrdinaryChar('b'));
        assert_eq!(result[2], Directive::OrdinaryChar('c'));
    }

    #[test]
    fn test_complex_format() {
        let result = parse_scanf_format("%2$*'10lld %s").unwrap();
        assert_eq!(result.len(), 3);

        match &result[0] {
            Directive::ConversionSpec(spec) => {
                assert_eq!(spec.nth, Some(2));
                assert!(spec.suppress_assignment);
                assert!(spec.thousands_separator);
                assert!(!spec.implicit_allocation);
                assert_eq!(spec.width, Some(10));
                assert_eq!(spec.type_modifier, Some(('l', 'l')));
                assert_eq!(spec.specifier, 'd');
            }
            _ => panic!("Expected ConversionSpec"),
        }

        assert_eq!(result[1], Directive::ZeroOrMoreWhitespace);

        match &result[2] {
            Directive::ConversionSpec(spec) => {
                assert_eq!(spec.specifier, 's');
            }
            _ => panic!("Expected ConversionSpec"),
        }
    }

    #[test]
    fn test_complex_format_bad() {
        let result = parse_scanf_format("%2$*'m10lld %s");
        assert!(result.is_err());
    }

    #[test]
    fn test_bracket_expression() {
        let result = parse_scanf_format("%[0-9]").unwrap();
        assert_eq!(result.len(), 1);
        match &result[0] {
            Directive::ConversionSpec(spec) => {
                assert_eq!(spec.specifier, '[');
                assert_eq!(spec.bracket_content, Some("0-9".to_string()));
            }
            _ => panic!("Expected ConversionSpec"),
        }
    }

    #[test]
    fn test_literal_percent() {
        let result = parse_scanf_format("%%").unwrap();
        assert_eq!(result.len(), 1);
        match &result[0] {
            Directive::OrdinaryChar(c) => {
                assert_eq!(*c, '%');
            }
            _ => panic!("Expected OrdinaryChar"),
        }
    }
}
