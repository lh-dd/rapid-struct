## rapid-struct

<img width="1284" height="809" alt="image" src="https://github.com/user-attachments/assets/946f7089-d0eb-47d7-882f-fb592fceddeb" />

<img width="1284" height="809" alt="image" src="https://github.com/user-attachments/assets/f952e3c6-92a9-42db-87f8-375c26d8b0af" />

## Description

A lightweight binary editor for Windows. Edit binary files using simple text-based structure definitions.

## .rs definition file

Format:

#comments<br>
name, description, offset, type


Supports all common types:

str(n), hex(n), i8, u8, i16, u16, i32, u32, i64, u64

Group nodes:

@group "group name"<br>
name, description, offset, type<br>
name, description, offset, type<br>
@endgroup
