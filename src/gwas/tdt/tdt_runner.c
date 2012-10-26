/*
 * Copyright (c) 2012 Cristina Yenyxe Gonzalez Garcia (ICM-CIPF)
 * Copyright (c) 2012 Ignacio Medina (ICM-CIPF)
 *
 * This file is part of hpg-variant.
 *
 * hpg-variant is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * hpg-variant is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with hpg-variant. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tdt_runner.h"

// int run_tdt_test(shared_options_data_t* shared_options_data, tdt_options_data_t* options_data) {
int run_tdt_test(shared_options_data_t* shared_options_data) {
    list_t *read_list = (list_t*) malloc(sizeof(list_t));
    list_init("text", 1, shared_options_data->max_batches, read_list);
    list_t *output_list = (list_t*) malloc (sizeof(list_t));
    list_init("output", shared_options_data->num_threads, INT_MAX, output_list);

    int ret_code = 0;
    vcf_file_t *file = vcf_open(shared_options_data->vcf_filename, shared_options_data->max_batches);
    if (!file) {
        LOG_FATAL("VCF file does not exist!\n");
    }
    
    ped_file_t *ped_file = ped_open(shared_options_data->ped_filename);
    if (!ped_file) {
        LOG_FATAL("PED file does not exist!\n");
    }
    
    size_t output_directory_len = strlen(shared_options_data->output_directory);
    
    LOG_INFO("About to read PED file...\n");
    // Read PED file before doing any proccessing
    ret_code = ped_read(ped_file);
    if (ret_code != 0) {
        LOG_FATAL_F("Can't read PED file: %s\n", ped_file->filename);
    }
    
    // Try to create the directory where the output files will be stored
    ret_code = create_directory(shared_options_data->output_directory);
    if (ret_code != 0 && errno != EEXIST) {
        LOG_FATAL_F("Can't create output directory: %s\n", shared_options_data->output_directory);
    }
    
    LOG_INFO("About to perform TDT test...\n");

#pragma omp parallel sections private(ret_code)
    {
#pragma omp section
        {
            printf("Level %d: number of threads in the team - %d\n", 0, omp_get_num_threads());
            
 //           LOG_DEBUG_F("Thread %d reads the VCF file\n", omp_get_thread_num());
            // Reading
            double start = omp_get_wtime();

            ret_code = 0;
            if (shared_options_data->batch_bytes > 0) {
                ret_code = vcf_read_batches_in_bytes(read_list, shared_options_data->batch_bytes, file);
            } else if (shared_options_data->batch_lines > 0) {
                ret_code = vcf_read_batches(read_list, shared_options_data->batch_lines, file);
            }

            double stop = omp_get_wtime();

            if (ret_code) {
                LOG_FATAL_F("Error %d while reading the file %s\n", ret_code, file->filename);
            }

            LOG_INFO_F("[%dR] Time elapsed = %f s\n", omp_get_thread_num(), stop - start);
            LOG_INFO_F("[%dR] Time elapsed = %e ms\n", omp_get_thread_num(), (stop - start) * 1000);

            list_decr_writers(read_list);
        }

#pragma omp section
        {
            printf("Level %d: number of threads in the team - %d\n", 10, omp_get_num_threads());
            double *res = calloc (shared_options_data->num_threads, sizeof(double));
            
            // Enable nested parallelism
            omp_set_nested(1);
            
 //           LOG_DEBUG_F("Thread %d processes data\n", omp_get_thread_num());
            
            volatile int initialization_done = 0;
            cp_hashtable *sample_ids = NULL;
            
            // Create chain of filters for the VCF file
            filter_t **filters = NULL;
            int num_filters = 0;
            if (shared_options_data->chain != NULL) {
                filters = sort_filter_chain(shared_options_data->chain, &num_filters);
            }
            FILE *passed_file = NULL, *failed_file = NULL;
            get_output_files(shared_options_data, &passed_file, &failed_file);
    
            double start = omp_get_wtime();
            
#pragma omp parallel num_threads(shared_options_data->num_threads) shared(initialization_done, sample_ids, filters)
            {
            printf("Level %d: number of threads in the team - %d\n", 11, omp_get_num_threads()); 
            
            family_t **families = (family_t**) cp_hashtable_get_values(ped_file->families);
            int num_families = get_num_families(ped_file);
            
            int i = 0;
            list_item_t *item = NULL;
            while ((item = list_remove_item(read_list)) != NULL) {
                
//                 double start_batch, end_batch;
//                 start_batch = omp_get_wtime();

                char *text_begin = (char*) item->data_p;
                char *text_end_batch = text_begin + strlen(text_begin);
                
                assert(text_end_batch != NULL);
                
//                 start_batch = omp_get_wtime();
                
                vcf_reader_status *status = vcf_reader_status_new(shared_options_data->batch_lines);
                if (shared_options_data->batch_bytes > 0) {
                    ret_code = run_vcf_parser(text_begin, text_end_batch, 0, file, status);
                } else if (shared_options_data->batch_lines > 0) {
                    ret_code = run_vcf_parser(text_begin, text_end_batch, shared_options_data->batch_lines, file, status);
                }
                
                if (ret_code > 0) {
                    LOG_FATAL_F("Error %d while parsing the file %s\n", ret_code, file->filename);
                }
                
//                 end_batch = omp_get_wtime();
//                 printf("WT%d end_batch ragel\t%f s\n", omp_get_thread_num(), end_batch - start_batch);
                
                // Initialize structures needed for TDT and write headers of output files
                if (!initialization_done) {
# pragma omp critical
                {
                    // Guarantee that just one thread performs this operation
                    if (!initialization_done) {
                        // Create map to associate the position of individuals in the list of samples defined in the VCF file
                        sample_ids = associate_samples_and_positions(file);
                        
                        // Write file format, header entries and delimiter
                        if (passed_file != NULL) { write_vcf_header(file, passed_file); }
                        if (failed_file != NULL) { write_vcf_header(file, failed_file); }
                        
                        LOG_DEBUG("VCF header written\n");
                        
                        initialization_done = 1;
                    }
                }
                }
                
                vcf_batch_t *batch = fetch_vcf_batch(file);
                array_list_t *input_records = batch->records;
                array_list_t *passed_records = NULL, *failed_records = NULL;

                if (i % 100 == 0) {
                    LOG_INFO_F("Batch %d reached by thread %d - %zu/%zu records \n", 
                            i, omp_get_thread_num(),
                            batch->records->size, batch->records->capacity);
                }

                if (filters == NULL) {
                    passed_records = input_records;
                } else {
                    failed_records = array_list_new(input_records->size + 1, 1, COLLECTION_MODE_ASYNCHRONIZED);
                    passed_records = run_filter_chain(input_records, failed_records, filters, num_filters);
                }

                // Launch TDT test over records that passed the filters
                if (passed_records->size > 0) {
//                     printf("[%d] first chromosome pos = %lu\n", omp_get_thread_num(), ((vcf_record_t*) passed_records->items[0])->position);
                    ret_code = tdt_test((vcf_record_t**) passed_records->items, passed_records->size, families, num_families, sample_ids, output_list);
                    if (ret_code) {
                        LOG_FATAL_F("[%d] Error in execution #%d of TDT\n", omp_get_thread_num(), i);
                    }
                }
                
                // Write records that passed and failed to separate files
                if (passed_file != NULL && failed_file != NULL) {
                    if (passed_records != NULL && passed_records->size > 0) {
                #pragma omp critical 
                    {
                        for (int r = 0; r < passed_records->size; r++) {
                            write_vcf_record(passed_records->items[r], passed_file);
                        }
//                         write_batch(passed_records, passed_file);
                    }
                    }
                    if (failed_records != NULL && failed_records->size > 0) {
                #pragma omp critical 
                    {
                        for (int r = 0; r < passed_records->size; r++) {
                            write_vcf_record(failed_records->items[r], failed_file);
                        }
//                         write_batch(failed_records, failed_file);
                    }
                    }
                }
                
                // Free items in both lists (not their internal data)
                if (passed_records != input_records) {
         //           LOG_DEBUG_F("[Batch %d] %zu passed records\n", i, passed_records->size);
                    array_list_free(passed_records, NULL);
                }
                if (failed_records) {
         //           LOG_DEBUG_F("[Batch %d] %zu failed records\n", i, failed_records->size);
                    array_list_free(failed_records, NULL);
                }
                
                // Free batch and its contents
                vcf_reader_status_free(status);
                vcf_batch_free(batch);
                list_item_free(item);
                
                i++;
                
            }
            
//             list_decr_writers(vcf_batches_list);
            list_decr_writers(file->record_batches);
            }

            double stop = omp_get_wtime();
            
            LOG_INFO_F("[%d] Time elapsed = %f s\n", omp_get_thread_num(), stop - start);
            LOG_INFO_F("[%d] Time elapsed = %e ms\n", omp_get_thread_num(), (stop - start) * 1000);

            // Free resources
            if (sample_ids) { cp_hashtable_destroy(sample_ids); }
            
            LOG_INFO_F("[%d] Time elapsed = %f s\n", omp_get_thread_num(), stop - start);
            LOG_INFO_F("[%d] Time elapsed = %e ms\n", omp_get_thread_num(), (stop - start) * 1000);

//             // Free resources
//             if (sample_ids) { cp_hashtable_destroy(sample_ids); }
//             
//             // Free filters
//             for (int i = 0; i < num_filters; i++) {
//                 filter_t *filter = filters[i];
//                 filter->free_func(filter);
//             }
//             free(filters);
            
            // Decrease list writers count
            for (int i = 0; i < shared_options_data->num_threads; i++) {
                list_decr_writers(output_list);
            }
        }

#pragma omp section
        {
            printf("Level %d: number of threads in the team - %d\n", 20, omp_get_num_threads());
            
            // Thread which writes the results to the output file
            FILE *fd = NULL;    // TODO check if output file is defined
            char *path = NULL;
            char *filename = strdup("hpg-variant.tdt");
            size_t filename_len = strlen(filename);
            
            // Set whole path to the output file
            path = (char*) calloc ((output_directory_len + filename_len + 2), sizeof(char));
            sprintf(path, "%s/%s", shared_options_data->output_directory, filename);
            fd = fopen(path, "w");
            
            LOG_INFO_F("TDT output filename = %s\n", path);
            free(filename);
//             free(path);
            
            double start = omp_get_wtime();
            
            // Write data: header + one line per variant
            list_item_t* item = NULL;
            tdt_result_t *result;
            fprintf(fd, "#CHR         POS       A1      A2         T       U           OR           CHISQ         P-VALUE\n");
            while ((item = list_remove_item(output_list)) != NULL) {
                result = item->data_p;
                
                fprintf(fd, "%s\t%8ld\t%s\t%s\t%3d\t%3d\t%6f\t%6f\t%6f\n",
                       result->chromosome, result->position, result->reference, result->alternate, 
                       result->t1, result->t2, result->odds_ratio, result->chi_square, result->p_value);
                
                tdt_result_free(result);
                list_item_free(item);
            }
            
            fclose(fd);
            
            // Sort resulting file
            char *cmd = calloc (40 + strlen(path) * 4, sizeof(char));
            sprintf(cmd, "sort -k1,1h -k2,2n %s > %s.tmp && mv %s.tmp %s", path, path, path, path);
            
            int sort_ret = system(cmd);
            if (sort_ret) {
                LOG_WARN("TDT results could not be sorted by chromosome and position, will be shown unsorted\n");
            }
            
            free(cmd);
            free(path);
            
            double stop = omp_get_wtime();

            LOG_INFO_F("[%dW] Time elapsed = %f s\n", omp_get_thread_num(), stop - start);
            LOG_INFO_F("[%dW] Time elapsed = %e ms\n", omp_get_thread_num(), (stop - start) * 1000);

        }
    }
    
    free(read_list);
    free(output_list);
    vcf_close(file);
    // TODO delete conflicts among frees
//     ped_close(ped_file, 0);
    
    
    return ret_code;
}


cp_hashtable* associate_samples_and_positions(vcf_file_t* file) {
    LOG_DEBUG_F("** %zu sample names read\n", file->samples_names->size);
    array_list_t *sample_names = file->samples_names;
    cp_hashtable *sample_ids = cp_hashtable_create_by_option(COLLECTION_MODE_NOSYNC,
                                                             sample_names->size * 2,
                                                             cp_hash_string,
                                                             (cp_compare_fn) strcasecmp,
                                                             NULL,
                                                             NULL,
                                                             NULL,
                                                             NULL
                                                            );
    
    int *index;
    char *name;
    for (int i = 0; i < sample_names->size; i++) {
        name = sample_names->items[i];
        index = (int*) malloc (sizeof(int)); *index = i;
        cp_hashtable_put(sample_ids, name, index);
    }
//     char **keys = (char**) cp_hashtable_get_keys(sample_names);
//     int num_keys = cp_hashtable_count(sample_names);
//     for (int i = 0; i < num_keys; i++) {
//         printf("%s\t%d\n", keys[i], *((int*) cp_hashtable_get(sample_ids, keys[i])));
//     }
    
    return sample_ids;
}